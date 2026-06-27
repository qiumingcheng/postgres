/*-------------------------------------------------------------------------
 * memo.c
 *    Cascades Memo 结构：Group / GroupExpression / 初始化 / 去重
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/cost.h"
#include "nodes/bitmapset.h"
#include "utils/memutils.h"
#include "utils/hsearch.h"
#include "utils/syscache.h"
#include "utils/selfuncs.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "access/heapam.h"

/* ========================================================================
 * Hash Table for GroupExpression Dedup
 * ======================================================================== */

__attribute__((noinline)) uint32
pg_memo_hash_key(const void *key_ptr, Size keysize)
{
    const PgExprHashKey *key = (const PgExprHashKey *) key_ptr;
    uint32 h = (uint32) key->op;
    int i;

    for (i = 0; i < key->num_inputs && i < PG_MEMO_HASH_MAX_INPUTS; i++)
    {
        h = (h << 5) | (h >> 27);
        h ^= (uint32) key->group_ids[i];
    }
    return h;
}

__attribute__((noinline)) int
pg_memo_match_key(const void *key1, const void *key2, Size keysize)
{
    const PgExprHashKey *a = (const PgExprHashKey *) key1;
    const PgExprHashKey *b = (const PgExprHashKey *) key2;
    int i;

    if (a->op != b->op || a->num_inputs != b->num_inputs)
        return 1;
    for (i = 0; i < a->num_inputs && i < PG_MEMO_HASH_MAX_INPUTS; i++)
    {
        if (a->group_ids[i] != b->group_ids[i])
            return 1;
    }
    return 0;
}

static void
pg_memo_build_hash_key(PgExprHashKey *key, PgGroupExpr *expr)
{
    ListCell *lc;
    int i = 0;

    MemSet(key, 0, sizeof(PgExprHashKey));
    key->op = expr->op;
    key->num_inputs = list_length(expr->inputs);
    foreach(lc, expr->inputs)
    {
        if (i >= PG_MEMO_HASH_MAX_INPUTS)
            break;
        key->group_ids[i++] = ((PgMemoGroup *) lfirst(lc))->id;
    }
    /* Pad with -1 so unused slots don't match by accident */
    while (i < PG_MEMO_HASH_MAX_INPUTS)
        key->group_ids[i++] = -1;
    /* Update expr hash for quick comparison */
    expr->expr_hash = pg_memo_hash_key(key, sizeof(PgExprHashKey));
}

/* ========================================================================
 * Group 操作
 * ======================================================================== */

PgMemoGroup *
pg_memo_new_group(PgPlannerCascadesContext *ctx)
{
    PgMemoGroup *group = (PgMemoGroup *) palloc0(sizeof(PgMemoGroup));

    group->id = list_length(ctx->memo->groups);
    group->logical_exprs = NIL;
    group->physical_exprs = NIL;
    group->best_entries = NIL;
    group->rows = 0;
    group->width = 0;
    group->rel = NULL;

    ctx->memo->groups = lappend(ctx->memo->groups, group);
    return group;
}

PgGroupExpr *
pg_memo_new_group_expr(PgPlannerCascadesContext *ctx, PgCascadesOpKind op)
{
    PgGroupExpr *expr = (PgGroupExpr *) palloc0(sizeof(PgGroupExpr));

    expr->op = op;
    expr->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    expr->inputs = NIL;
    expr->applied_rules = NULL;
    expr->explored_rules = NULL;
    expr->stats_derived = false;
    expr->op_private = NULL;
    expr->owner_group = NULL;

    return expr;
}

void
pg_memo_add_physical_expr(PgMemoGroup *group, PgGroupExpr *expr)
{
    expr->owner_group = group;
    group->physical_exprs = lappend(group->physical_exprs, expr);
}

void
pg_memo_add_logical_expr(PgMemoGroup *group, PgGroupExpr *expr)
{
    expr->owner_group = group;
    group->logical_exprs = lappend(group->logical_exprs, expr);
}

/* ========================================================================
 * Memo 初始化
 * ======================================================================== */

/*
 * pg_memo_insert_expression:
 *   将 expression 插入 Memo。
 *
 *   inputs 可以是两种形式：
 *     a) PgMemoGroup *（直接 Group 引用，来自 pg_adapter 构建的逻辑树）
 *     b) PgGroupExpr *（子 expression，需要递归，来自 rule transform）
 *
 *   返回该 expression 所在的 Group。
 */
PgMemoGroup *
pg_memo_insert_expression(PgPlannerCascadesContext *ctx,
                          PgMemo *memo,
                          PgGroupExpr *expr,
                          PgMemoGroup *parent_group)
{
    PgMemoGroup *group;
    PgExprHashKey key;
    bool found;

    /*
     * Build hash key from op + input group IDs.
     * This also sets expr->expr_hash.
     */
    pg_memo_build_hash_key(&key, expr);

    /*
     * Phase 4 + Phase 6: Global hash table dedup for logical expressions.
     *
     * Phase 7: Skip dedup for LOGICAL_SCAN leaf expressions (inputs=NIL).
     * Two scans on different tables have identical hash keys
     * (op + 0 children) but are NOT semantically equivalent.
     *
     * Phase 6: When a duplicate expression is found in a DIFFERENT group
     * than its first occurrence, automatically merge the two groups.
     * This is the StarRocks "copyIn auto-merge" behavior:
     *   if the same (op + child groups) expression appears in two groups,
     *   those groups are semantically equivalent → merge them.
     */
    if ((int)expr->op < PG_CASCADES_PHYSICAL_SEQSCAN &&
        expr->mode != PG_PHYS_EXPR_IMPORTED_PATH &&
        memo->group_expr_table != NULL &&
        !(expr->op == PG_CASCADES_LOGICAL_SCAN && expr->inputs == NIL))
    {
        PgExprHashEntry *entry;

        entry = (PgExprHashEntry *)
            hash_search(memo->group_expr_table, &key, HASH_ENTER, &found);

        if (found)
        {
            /*
             * Duplicate found in global hash table.
             * entry->owner_group_id tells us which group owns the first copy.
             */
            PgMemoGroup *owner_group = NULL;
            ListCell    *gc;

            foreach(gc, memo->groups)
            {
                PgMemoGroup *g = (PgMemoGroup *) lfirst(gc);
                if (g->id == entry->owner_group_id)
                {
                    owner_group = g;
                    break;
                }
            }

            if (owner_group == NULL)
                return NULL;  /* shouldn't happen, but be safe */

            /*
             * Set owner_group on the expression even when returning
             * an existing group. The task scheduler needs expr->owner_group
             * to be non-NULL for OPTIMIZE_EXPRESSION tasks.
             */
            expr->owner_group = owner_group;

            /*
             * If a parent_group is given AND it differs from the owner,
             * the same expression exists in two groups — they are equivalent.
             * Merge the parent (typically the rule's target) into the owner.
             */
            if (parent_group != NULL && parent_group != owner_group)
            {
                pg_memo_merge_group(ctx, owner_group, parent_group);
                if (ctx->debug)
                    elog(NOTICE, "Cascades: auto-merged group %d into group %d "
                         "(duplicate expr op=%d)",
                         parent_group->id, owner_group->id, (int)expr->op);
            }

            return owner_group;  /* use the existing group */
        }
        else
        {
            /* First occurrence — will set owner_group_id after group is determined */
        }
    }

    /*
     * Determine target group:
     * - If parent_group is provided (rule transform), use it.
     * - Otherwise create a new group (pg_adapter building logical tree).
     */
    if (parent_group != NULL)
    {
        group = parent_group;

        /*
         * Phase 4: Local dedup — check if an equivalent logical expression
         * already exists in this group.  This closes the loop where
         * JoinCommutativity would otherwise create A⋈B → B⋈A → A⋈B → ...
         * infinitely.  Global hash table handles cross-group dedup.
         */
        if ((int)expr->op < PG_CASCADES_PHYSICAL_SEQSCAN)
        {
            ListCell *lc;
            foreach(lc, group->logical_exprs)
            {
                PgGroupExpr *existing = (PgGroupExpr *) lfirst(lc);
                if (existing->expr_hash == expr->expr_hash &&
                    existing->op == expr->op)
                {
                    /* Already have this expression — skip */
                    return group;
                }
            }
        }
    }
    else
        group = pg_memo_new_group(ctx);

    /*
     * Phase 6: If this was the first occurrence of this expression,
     * record the group ID in the hash entry for future auto-merge.
     * Phase 7: Skip for LOGICAL_SCAN leaf expressions (same reason as above).
     */
    if ((int)expr->op < PG_CASCADES_PHYSICAL_SEQSCAN &&
        expr->mode != PG_PHYS_EXPR_IMPORTED_PATH &&
        memo->group_expr_table != NULL &&
        !(expr->op == PG_CASCADES_LOGICAL_SCAN && expr->inputs == NIL))
    {
        PgExprHashEntry *entry;
        bool             dummy_found;

        entry = (PgExprHashEntry *)
            hash_search(memo->group_expr_table, &key, HASH_FIND, &dummy_found);
        if (entry != NULL && !found)
        {
            /* entry was newly inserted by HASH_ENTER above — set owner */
            entry->owner_group_id = group->id;
        }
    }

    /* Add as logical or physical */
    if (expr->mode == PG_PHYS_EXPR_IMPORTED_PATH ||
        (int)expr->op >= PG_CASCADES_PHYSICAL_SEQSCAN)
        pg_memo_add_physical_expr(group, expr);
    else
        pg_memo_add_logical_expr(group, expr);

    return group;
}

/*
 * pg_memo_insert_expression_tree:
 *    Convert a standalone PgGroupExpr * tree (from pg_cascades_build_initial_tree)
 *    into proper Memo groups.  In the standalone tree, expr->inputs are
 *    PgGroupExpr * (child expressions).  This function recursively creates
 *    PgMemoGroup entries and replaces the inputs with PgMemoGroup * references.
 *
 *    After this call, the tree is fully in Memo and the rewrite pipeline
 *    can operate on it via ctx->memo->root_group.
 */
PgMemoGroup *
pg_memo_insert_expression_tree(PgPlannerCascadesContext *ctx,
                                PgGroupExpr *tree_root)
{
    PgMemo *memo = ctx->memo;
    ListCell *lc;
    List     *new_inputs = NIL;
    PgMemoGroup *group;

    if (tree_root == NULL)
        return NULL;

    /* Step 1: Recursively process children first (bottom-up) */
    foreach(lc, tree_root->inputs)
    {
        PgGroupExpr *child_expr = (PgGroupExpr *) lfirst(lc);
        PgMemoGroup *child_group;

        child_group = pg_memo_insert_expression_tree(ctx, child_expr);
        if (child_group != NULL)
            new_inputs = lappend(new_inputs, child_group);
    }

    /* Step 2: Replace inputs with PgMemoGroup * references */
    tree_root->inputs = new_inputs;

    /* Step 3: Insert this expression into Memo */
    group = pg_memo_insert_expression(ctx, memo, tree_root, NULL);

    /*
     * Phase 6: If this is a LogicalScan whose op_private points to a
     * valid PG RelOptInfo, bridge the standalone OptExpression tree
     * with PG's lower path generation (make_one_rel).  Populate the
     * group with physical path candidates and set rel/rows/width.
     *
     * Without this, tree-created groups have NULL rel pointers which
     * causes SIGSEGV when the task scheduler dereferences group->rel.
     */
    if (group != NULL && tree_root->op == PG_CASCADES_LOGICAL_SCAN &&
        tree_root->op_private != NULL)
    {
        RelOptInfo *rel = (RelOptInfo *) tree_root->op_private;
        ListCell   *lc;

        group->rel = rel;
        group->rows = rel->rows;
        group->width = rel->width;

        /* Phase 6: populate per-column statistics */
        pg_statistics_populate_columns(group);

        /* Import PG paths as physical expression candidates */
        foreach(lc, rel->pathlist)
        {
            Path            *path = (Path *) lfirst(lc);
            PgGroupExpr     *phys_expr;
            PgCascadesOpKind op;

            op = pg_cascades_pathtype_to_opkind(path->pathtype);
            if ((int) op < 0)
                continue;

            phys_expr = pg_memo_new_group_expr(ctx, op);
            phys_expr->mode = PG_PHYS_EXPR_IMPORTED_PATH;
            phys_expr->op_private = path;
            phys_expr->inputs = NIL;

            pg_memo_add_physical_expr(group, phys_expr);
        }
    }

    if (group != NULL && tree_root->op == PG_CASCADES_LOGICAL_JOIN &&
        list_length(tree_root->inputs) == 2)
    {
        PgMemoGroup *og = (PgMemoGroup *) linitial(tree_root->inputs);
        PgMemoGroup *ig = (PgMemoGroup *) lsecond(tree_root->inputs);
        if (og != NULL && ig != NULL && og->rel != NULL && ig->rel != NULL)
        {
            Relids jr = bms_union(og->rel->relids, ig->rel->relids);
            ListCell *jlc;
            foreach(jlc, ctx->root->join_rel_list)
            {
                RelOptInfo *jr2 = (RelOptInfo *) lfirst(jlc);
                if (bms_equal(jr2->relids, jr))
                {
                    ListCell *pc;
                    group->rel = jr2;
                    if (group->rows <= 0) group->rows = jr2->rows;
                    if (group->width <= 0) group->width = jr2->width;
                    foreach(pc, jr2->pathlist)
                    {
                        Path *p = (Path *) lfirst(pc);
                        PgGroupExpr *pe;
                        PgCascadesOpKind op = pg_cascades_pathtype_to_opkind(p->pathtype);
                        if ((int) op < 0) continue;
                        pe = pg_memo_new_group_expr(ctx, op);
                        pe->mode = PG_PHYS_EXPR_IMPORTED_PATH;
                        pe->op_private = p;
                        pe->inputs = NIL;
                        pg_memo_add_physical_expr(group, pe);
                    }
                    break;
                }
            }
            bms_free(jr);
        }
    }

    /* Set root_group if this is the top-level call */
    if (group != NULL)
        memo->root_group = group;

    return group;
}

/* ========================================================================
 * Memo Initialization from Query Tree
 *
 *   StarRocks equivalent: Memo.init(logicOperatorTree)
 *
 *   1. Allocate Memo + hash table for GroupExpression dedup
 *   2. Build standalone OptExpression tree from Query + PG paths
 *   3. Recursively insert tree into Memo as Groups + GroupExpressions
 *
 *   Sets ctx->memo and ctx->memo->root_group on success.
 * ======================================================================== */
void
pg_memo_init_from_tree(PgPlannerCascadesContext *ctx)
{
    PgMemo     *memo;
    HASHCTL     hash_ctl;
    PgGroupExpr *opt_tree;
    MemoryContext old_cxt;

    /* Step 1: Create Memo shell with hash table */
    old_cxt = MemoryContextSwitchTo(ctx->memo_cxt);

    memo = (PgMemo *) palloc0(sizeof(PgMemo));
    memo->context = ctx->memo_cxt;
    memo->groups = NIL;

    MemSet(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(PgExprHashKey);
    hash_ctl.entrysize = sizeof(PgExprHashEntry);
    hash_ctl.hcxt = ctx->memo_cxt;
    memo->group_expr_table = hash_create("Memo GroupExpr Table", 256,
                                          &hash_ctl,
                                          HASH_ELEM | HASH_CONTEXT);

    ctx->memo = memo;
    MemoryContextSwitchTo(old_cxt);

    /* Step 2: Build standalone OptExpression tree from PG Query + paths */
    opt_tree = pg_cascades_build_initial_tree(ctx);

    /* Step 3: Recursively insert tree into Memo */
    pg_memo_insert_expression_tree(ctx, opt_tree);
}

/* ========================================================================
 * Logical Property 推导
 * ======================================================================== */

/*
 * pg_memo_derive_logical_property:
 *   递归推导每个 Group 的逻辑属性（rows, width）。
 *   对应 StarRocks 的 deriveAllGroupLogicalProperty()。
 */
void
pg_memo_derive_logical_property(PgMemo *memo, PgMemoGroup *group,
                                 PgPlannerCascadesContext *ctx)
{
    ListCell   *lc;

    if (group->rows > 0)
        return;                         /* 已推导 */

    foreach(lc, group->logical_exprs)
    {
        PgGroupExpr *expr = (PgGroupExpr *) lfirst(lc);
        PgMemoGroup *child;
        ListCell   *cl;

        /* 先递归推导所有 child */
        foreach(cl, expr->inputs)
        {
            child = (PgMemoGroup *) lfirst(cl);
            pg_memo_derive_logical_property(memo, child, ctx);
        }

        /* 根据 op kind 推导当前 expression 的 rows/width */
        switch (expr->op)
        {
            case PG_CASCADES_LOGICAL_SCAN:
                if (expr->op_private != NULL)
                {
                    RelOptInfo *rel = (RelOptInfo *) expr->op_private;
                    group->rows = rel->rows;
                    group->width = rel->width;
                }
                break;

            case PG_CASCADES_LOGICAL_JOIN:
                if (list_length(expr->inputs) >= 2)
                {
                    child = (PgMemoGroup *) linitial(expr->inputs);
                    group->rows = child->rows;
                    group->width = child->width;

                    child = (PgMemoGroup *) lsecond(expr->inputs);
                    group->rows *= child->rows * 0.1;
                    group->width += child->width;
                }
                break;

            case PG_CASCADES_LOGICAL_PROJECT:
            case PG_CASCADES_LOGICAL_FILTER:
            case PG_CASCADES_LOGICAL_DISTINCT:
            case PG_CASCADES_LOGICAL_SORT:
            case PG_CASCADES_LOGICAL_LIMIT:
                /* 透传 child */
                if (list_length(expr->inputs) >= 1)
                {
                    child = (PgMemoGroup *) linitial(expr->inputs);
                    group->rows = child->rows;
                    group->width = child->width;
                }
                break;

            case PG_CASCADES_LOGICAL_AGG:
                if (ctx->upper != NULL)
                {
                    group->rows = ctx->upper->dNumGroups;
                    group->width = 0;
                }
                break;

            default:
                break;
        }

        /* 只取第一个 logical expression 的结果 */
        break;
    }
}

/*
 * pg_derive_expr_stats:
 *    Derive per-expression row count and width estimates.
 *
 *    Uses PG's existing statistics (RelOptInfo.rows/width,
 *    clauselist_selectivity, dNumGroups) for cost-accurate estimates.
 *    Called both by pg_memo_derive_logical_property_v2 (group-level)
 *    and pg_task_derive_stats (per-expression-level).
 */
void
pg_derive_expr_stats(PgPlannerCascadesContext *ctx, PgGroupExpr *expr,
                     double *out_rows, int *out_width)
{
    double rows = 0;
    int    width = 0;

    switch (expr->op)
    {
        case PG_CASCADES_LOGICAL_SCAN:
            if (expr->owner_group && expr->owner_group->rel)
            {
                RelOptInfo *rel = expr->owner_group->rel;
                rows = rel->rows;
                width = rel->width;
            }
            else if (expr->op_private != NULL)
            {
                RelOptInfo *rel = (RelOptInfo *) expr->op_private;
                rows = rel->rows;
                width = rel->width;
            }
            break;

        case PG_CASCADES_LOGICAL_JOIN:
            {
                double outer_rows = 1000, inner_rows = 1000;
                int    outer_width = 10, inner_width = 10;
                double sel = 0.1;

                if (list_length(expr->inputs) >= 2)
                {
                    PgMemoGroup *outer = (PgMemoGroup *) linitial(expr->inputs);
                    PgMemoGroup *inner = (PgMemoGroup *) lsecond(expr->inputs);

                    if (outer->rows > 0) outer_rows = outer->rows;
                    if (inner->rows > 0) inner_rows = inner->rows;
                    if (outer->width > 0) outer_width = outer->width;
                    if (inner->width > 0) inner_width = inner->width;
                }

                /* Try PG's clauselist_selectivity for better estimate */
                {
                    PgJoinPrivate *jp = (PgJoinPrivate *) expr->op_private;
                    if (jp && jp->restrictlist != NIL)
                    {
                        Selectivity pg_sel;
                        pg_sel = clauselist_selectivity(ctx->root,
                                                         jp->restrictlist, 0,
                                                         jp->jointype, NULL);
                        if (pg_sel > 0 && pg_sel <= 1.0)
                            sel = pg_sel;
                    }
                }

                rows = outer_rows * inner_rows * sel;
                if (rows < 1) rows = 1;
                width = outer_width + inner_width;
            }
            break;

        case PG_CASCADES_LOGICAL_FILTER:
            if (list_length(expr->inputs) >= 1)
            {
                PgMemoGroup *child = (PgMemoGroup *) linitial(expr->inputs);
                double child_rows = (child->rows > 0) ? child->rows : 1000;
                double sel = 0.5;
                List *quals = (List *) expr->op_private;

                /* Phase 6: use column stats to improve default selectivity */
                if (child->stats.num_columns > 0 && child->stats.columns != NULL)
                {
                    double avg_ndv = 0;
                    int i;
                    for (i = 0; i < child->stats.num_columns; i++)
                    {
                        PgColumnStat *cs = &child->stats.columns[i];
                        if (cs->n_distinct > 0 && cs->n_distinct < child_rows)
                            avg_ndv += cs->n_distinct;
                        else if (cs->n_distinct < 0)
                            avg_ndv += child_rows * (-cs->n_distinct);
                        else
                            avg_ndv += child_rows;
                    }
                    if (child->stats.num_columns > 0)
                        avg_ndv /= child->stats.num_columns;
                    if (avg_ndv > 1.0 && avg_ndv < child_rows)
                        sel = 1.0 / avg_ndv;  /* each distinct value selects 1/ndv rows */
                    if (sel < 0.001) sel = 0.001;
                    if (sel > 0.9)   sel = 0.9;
                }

                if (quals != NIL)
                {
                    Selectivity pg_sel;
                    pg_sel = clauselist_selectivity(ctx->root, quals, 0,
                                                     JOIN_INNER, NULL);
                    if (pg_sel > 0 && pg_sel <= 1.0)
                        sel = pg_sel;
                }

                rows = child_rows * sel;
                if (rows < 1) rows = 1;
                width = child->width;
            }
            break;

        case PG_CASCADES_LOGICAL_AGG:
            if (ctx->upper && ctx->upper->dNumGroups > 0)
                rows = ctx->upper->dNumGroups;
            else if (list_length(expr->inputs) >= 1)
            {
                PgMemoGroup *child = (PgMemoGroup *) linitial(expr->inputs);
                rows = (child->rows > 0) ? child->rows * 0.1 : 100;
            }
            break;

        case PG_CASCADES_LOGICAL_PROJECT:
        case PG_CASCADES_LOGICAL_DISTINCT:
        case PG_CASCADES_LOGICAL_SORT:
        case PG_CASCADES_LOGICAL_LIMIT:
            if (list_length(expr->inputs) >= 1)
            {
                PgMemoGroup *child = (PgMemoGroup *) linitial(expr->inputs);
                rows = child->rows;
                width = child->width;
            }
            break;

        default:
            break;
    }

    if (out_rows) *out_rows = rows;
    if (out_width) *out_width = width;
}

/*
 * pg_memo_derive_logical_property_v2:
 *    Phase 4: Derive PgLogicalProperty for every group bottom-up.
 *    Fills group->logical_prop with relids, output_columns, etc.
 */
void
pg_memo_derive_logical_property_v2(PgMemo *memo, PgPlannerCascadesContext *ctx)
{
    ListCell *lc;

    /* Bottom-up: iterate groups in reverse order (children first) */
    foreach(lc, memo->groups)
    {
        PgMemoGroup *group = (PgMemoGroup *) lfirst(lc);
        ListCell   *elc;
        PgLogicalProperty best_prop;

        /* Already derived? */
        if (group->logical_prop.relids != NULL)
            continue;

        MemSet(&best_prop, 0, sizeof(PgLogicalProperty));

        /* Iterate ALL logical expressions, take the best stats.
         * After group merges, a group may contain expressions of
         * different types (e.g. LOGICAL_AGG + LOGICAL_PROJECT).
         * The first expression might have rows=0 (dNumGroups not
         * yet estimated), but another expression in the same group
         * may derive proper stats from its child.
         */
        foreach(elc, group->logical_exprs)
        {
            PgGroupExpr *expr = (PgGroupExpr *) lfirst(elc);
            PgLogicalProperty prop;

            MemSet(&prop, 0, sizeof(PgLogicalProperty));

            switch (expr->op)
            {
                case PG_CASCADES_LOGICAL_SCAN:
                    if (group->rel != NULL)
                    {
                        prop.relids = bms_copy(group->rel->relids);
                        prop.rows = group->rel->rows;
                        prop.width = group->rel->width;
                    }
                    break;

                case PG_CASCADES_LOGICAL_JOIN:
                    {
                        PgMemoGroup *outer, *inner;
                        if (list_length(expr->inputs) >= 2)
                        {
                            outer = (PgMemoGroup *) linitial(expr->inputs);
                            inner = (PgMemoGroup *) lsecond(expr->inputs);
                            prop.relids = bms_union(
                                bms_copy(outer->logical_prop.relids),
                                inner->logical_prop.relids);
                            prop.rows = outer->rows * inner->rows * 0.1;
                            prop.width = outer->width + inner->width;
                        }
                    }
                    break;

                case PG_CASCADES_LOGICAL_PROJECT:
                case PG_CASCADES_LOGICAL_FILTER:
                case PG_CASCADES_LOGICAL_DISTINCT:
                case PG_CASCADES_LOGICAL_SORT:
                case PG_CASCADES_LOGICAL_LIMIT:
                    if (list_length(expr->inputs) >= 1)
                    {
                        PgMemoGroup *child = (PgMemoGroup *) linitial(expr->inputs);
                        prop.relids = bms_copy(child->logical_prop.relids);
                        prop.rows = child->rows;
                        prop.width = child->width;
                    }
                    break;

                case PG_CASCADES_LOGICAL_AGG:
                    if (list_length(expr->inputs) >= 1)
                    {
                        PgMemoGroup *child = (PgMemoGroup *) linitial(expr->inputs);
                        prop.relids = bms_copy(child->logical_prop.relids);
                        /* dNumGroups may be 0 when estimation hasn't run yet;
                         * fall back to child's rows as a heuristic. */
                        if (ctx->upper != NULL && ctx->upper->dNumGroups > 0)
                            prop.rows = ctx->upper->dNumGroups;
                        else
                            prop.rows = child->rows * 0.1;  /* ~10% of input */
                    }
                    break;

                default:
                    break;
            }

            /*
             * Supplement with pg_derive_expr_stats for more accurate
             * row/width estimates (uses PG's clauselist_selectivity etc.)
             */
            {
                double derived_rows = 0;
                int    derived_width = 0;

                pg_derive_expr_stats(ctx, expr, &derived_rows, &derived_width);
                if (derived_rows > 0 && prop.rows <= 0)
                    prop.rows = derived_rows;
                if (derived_width > 0 && prop.width <= 0)
                    prop.width = derived_width;
            }

            /* Keep the best (largest rows) property across all expressions */
            if (prop.rows > best_prop.rows)
            {
                if (best_prop.relids != NULL)
                    bms_free(best_prop.relids);
                best_prop = prop;
            }
            else if (prop.relids != NULL)
                bms_free(prop.relids);
        }

        /* Store best derived property */
        group->logical_prop = best_prop;

        /*
         * Also propagate rows/width to the group itself.
         * After group merges, the target group may have rows=0
         * width=0 even though its logical expressions imply real
         * statistics.  The task scheduler's costing relies on
         * group->rows and group->width, so keep them in sync
         * with the derived logical property.
         */
        if (best_prop.rows > 0 && group->rows <= 0)
            group->rows = best_prop.rows;
        if (best_prop.width > 0 && group->width <= 0)
            group->width = best_prop.width;
    }
}

/* ========================================================================
 * Phase 6: Statistics API (StarRocks Statistics parity)
 *
 *   PgStatistics is standalone (not embedded in PgMemoGroup) to avoid
 *   struct layout changes.  Allocated via palloc, caller owns memory.
 * ======================================================================== */

__attribute__((noinline)) PgStatistics *
pg_statistics_from_group(PgMemoGroup *group)
{
    PgStatistics *s = (PgStatistics *) palloc0(sizeof(PgStatistics));

    if (group->rows > 0)
        s->row_count = group->rows;
    s->width   = group->width;
    s->derived = true;

    return s;
}

/*
 * pg_statistics_populate_columns:
 *   Populate per-column statistics for a base-table group from PG's
 *   RelOptInfo.  Reads avg_width from attr_widths; null_frac and
 *   n_distinct use defaults (Phase 2 will wire pg_statistic catalog).
 */
__attribute__((noinline)) void
pg_statistics_populate_columns(PgMemoGroup *group)
{
    RelOptInfo *rel = group->rel;
    int nattrs;
    int i;

    if (rel == NULL || rel->max_attr <= 0)
        return;

    nattrs = rel->max_attr;  /* user attributes: 1..max_attr */
    if (nattrs <= 0 || nattrs > 100)  /* safety cap */
        return;

    group->stats.columns = (PgColumnStat *)
        palloc0(sizeof(PgColumnStat) * nattrs);
    group->stats.num_columns = 0;

    for (i = 0; i < nattrs; i++)
    {
        AttrNumber attnum = (AttrNumber) (i + 1);
        PgColumnStat *cs = &group->stats.columns[group->stats.num_columns];
        HeapTuple   statsTuple;
        Form_pg_statistic staForm;
        float4     *numbers;
        int         nnumbers;

        cs->varattno = attnum;
        cs->vartype  = InvalidOid;
        cs->null_frac = 0.0;
        cs->n_distinct = -1.0;
        cs->avg_width = (rel->attr_widths != NULL && attnum <= rel->max_attr)
                         ? rel->attr_widths[attnum] : 8;
        if (cs->avg_width <= 0)
            cs->avg_width = 8;
        cs->hist_nvalues = 0;
        cs->hist_values  = NULL;

        /* Phase 6: read real statistics from pg_statistic catalog */
        statsTuple = SearchSysCache2(STATRELATTINH,
                                      ObjectIdGetDatum(rel->relid),
                                      Int16GetDatum(attnum));
        if (!HeapTupleIsValid(statsTuple))
        {
            group->stats.num_columns++;
            continue;
        }

        staForm = (Form_pg_statistic) GETSTRUCT(statsTuple);

        /* null_frac and n_distinct from pg_statistic */
        cs->null_frac  = staForm->stanullfrac;
        cs->n_distinct = staForm->stadistinct;

        /* Histogram: read STATISTIC_KIND_HISTOGRAM (kind=2) */
        if (get_attstatsslot(statsTuple,
                             0, 0,  /* type/typmod not needed for histogram */
                             STATISTIC_KIND_HISTOGRAM, InvalidOid,
                             &cs->hist_values, &cs->hist_nvalues,
                             &numbers, &nnumbers))
        {
            /* histogram stored in hist_values[0..hist_nvalues-1] */
            if (nnumbers > 0 && numbers != NULL)
                pfree(numbers);
        }

        ReleaseSysCache(statsTuple);
        group->stats.num_columns++;
    }
}

__attribute__((noinline)) PgStatistics *
pg_statistics_derive(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgStatistics *s = (PgStatistics *) palloc0(sizeof(PgStatistics));
    double rows = 0;
    int    width = 0;

    pg_derive_expr_stats(ctx, expr, &rows, &width);
    s->row_count = rows;
    s->width     = width;
    s->derived   = true;

    return s;
}

/* ========================================================================
 * Phase 5: Group-to-RelOptInfo mapping helpers
 * ======================================================================== */

/*
 * pg_cascades_group_to_rel:
 *   尝试将 PgMemoGroup 映射到 PG RelOptInfo。
 *   对 base rel group 直接返回 group->rel。
 *   对 join group：如果 children 都有 rel，调用 make_join_rel。
 */
RelOptInfo *
pg_cascades_group_to_rel(PgPlannerCascadesContext *ctx, PgMemoGroup *group)
{
    ListCell *lc;

    if (group->rel != NULL)
        return group->rel;

    /* 尝试从 children 推导 joinrel */
    foreach(lc, group->logical_exprs)
    {
        PgGroupExpr *expr = (PgGroupExpr *) lfirst(lc);

        if (expr->op != PG_CASCADES_LOGICAL_JOIN)
            continue;
        if (list_length(expr->inputs) != 2)
            continue;

        {
            PgMemoGroup *outer_grp = (PgMemoGroup *) linitial(expr->inputs);
            PgMemoGroup *inner_grp = (PgMemoGroup *) lsecond(expr->inputs);
            RelOptInfo *outer_rel = pg_cascades_group_to_rel(ctx, outer_grp);
            RelOptInfo *inner_rel = pg_cascades_group_to_rel(ctx, inner_grp);

            if (outer_rel != NULL && inner_rel != NULL)
            {
                RelOptInfo *joinrel;

                joinrel = make_join_rel(ctx->root, outer_rel, inner_rel);
                if (joinrel != NULL)
                {
                    set_cheapest(joinrel);
                    group->rel = joinrel;
                    return joinrel;
                }
            }
        }
    }

    return NULL;
}

/*
 * pg_cascades_group_relids:
 *   返回 group 涉及的 base rel OID 集合。
 */
Relids
pg_cascades_group_relids(PgPlannerCascadesContext *ctx, PgMemoGroup *group)
{
    Relids   result = NULL;
    ListCell *lc;

    if (group->rel != NULL)
        return bms_copy(group->rel->relids);

    /* join/upper group：从 children 合并 */
    foreach(lc, group->logical_exprs)
    {
        PgGroupExpr *expr = (PgGroupExpr *) lfirst(lc);
        ListCell   *ic;

        foreach(ic, expr->inputs)
        {
            PgMemoGroup *child = (PgMemoGroup *) lfirst(ic);
            Relids child_relids = pg_cascades_group_relids(ctx, child);

            if (child_relids != NULL)
                result = bms_union(result, child_relids);
        }
    }

    return result;
}

/* ========================================================================
 * Phase 5: Group Merging
 *
 * pg_memo_merge_group:
 *   Merge all logical expressions from source group into target group.
 *   This is used by rules like EliminateLimit (D3) and EliminateAgg (F1)
 *   where a no-op logical node's child group is equivalent to the
 *   current group — instead of creating a new expression that references
 *   the child, we move the child's expressions directly into the target.
 *
 *   After merging:
 *   - Source group's logical_exprs are appended to target's logical_exprs
 *   - Each merged expression's owner_group is updated to target
 *   - Source group is left empty (its expressions are now owned by target)
 *   - Target group inherits source group's rows/width/rel if unset
 *
 *   Important: This is a one-way operation. The source group is effectively
 *   "consumed" by the target group. This only makes sense for simple
 *   single-child no-op nodes like Limit (when no LIMIT) and Agg (when no
 *   aggregation).
 * ======================================================================== */

void
pg_memo_merge_group(PgPlannerCascadesContext *ctx,
                     PgMemoGroup *target, PgMemoGroup *source)
{
    ListCell *lc;
    ListCell *gc;

    if (source == NULL || target == NULL || source == target)
        return;

    /*
     * Phase 6: Before moving expressions, update all inputs references
     * in other groups that point to 'source' to instead point to 'target'.
     * Without this fix, tree-based Memo groups would have dangling
     * inputs pointers after merge, causing SIGSEGV in the task scheduler
     * when it tries to optimize an empty (merged-away) child group.
     *
     * IMPORTANT: Skip expressions in the TARGET group itself — those
     * intentionally reference the source (e.g., EliminateLimit merges
     * its child into itself, and the parent's expression inputs should
     * NOT be updated to self-reference, which would create a cycle).
     */
    foreach(gc, ctx->memo->groups)
    {
        PgMemoGroup *g = (PgMemoGroup *) lfirst(gc);
        ListCell   *ec;

        /* Skip target group — its expressions intentionally point to source */
        if (g == target)
            continue;

        foreach(ec, g->logical_exprs)
        {
            PgGroupExpr *expr = (PgGroupExpr *) lfirst(ec);
            ListCell   *ic;

            foreach(ic, expr->inputs)
            {
                if ((PgMemoGroup *) lfirst(ic) == source)
                    lfirst(ic) = target;
            }
        }
        foreach(ec, g->physical_exprs)
        {
            PgGroupExpr *expr = (PgGroupExpr *) lfirst(ec);
            ListCell   *ic;

            foreach(ic, expr->inputs)
            {
                if ((PgMemoGroup *) lfirst(ic) == source)
                    lfirst(ic) = target;
            }
        }
    }

    /* Move logical expressions from source to target */
    foreach(lc, source->logical_exprs)
    {
        PgGroupExpr *expr = (PgGroupExpr *) lfirst(lc);
        expr->owner_group = target;
        target->logical_exprs = lappend(target->logical_exprs, expr);
    }

    /* Move physical expressions too (if any) */
    foreach(lc, source->physical_exprs)
    {
        PgGroupExpr *expr = (PgGroupExpr *) lfirst(lc);
        expr->owner_group = target;
        target->physical_exprs = lappend(target->physical_exprs, expr);
    }

    /* Merge best entries */
    foreach(lc, source->best_entries)
    {
        PgGroupBestEntry *entry = (PgGroupBestEntry *) lfirst(lc);
        target->best_entries = lappend(target->best_entries, entry);
    }

    /* Inherit rows/width/rel from source if target doesn't have them */
    if (target->rows <= 0 && source->rows > 0)
        target->rows = source->rows;
    if (target->width <= 0 && source->width > 0)
        target->width = source->width;
    if (target->rel == NULL && source->rel != NULL)
        target->rel = source->rel;

    /*
     * Clear the target's logical property so derive_logical_property_v2
     * will re-derive it.  After merge, the target has new expressions
     * from the source, and the old cached property is stale.
     */
    MemSet(&target->logical_prop, 0, sizeof(PgLogicalProperty));

    /* Clear source (expressions are now owned by target) */
    source->logical_exprs = NIL;
    source->physical_exprs = NIL;
    source->best_entries = NIL;

    if (ctx->debug)
        elog(NOTICE, "Cascades: merged group %d into group %d",
             source->id, target->id);
}
