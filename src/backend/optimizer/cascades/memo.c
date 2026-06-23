/*-------------------------------------------------------------------------
 * memo.c
 *    Cascades Memo 结构：Group / GroupExpression / 初始化 / 去重
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "utils/memutils.h"
#include "utils/hsearch.h"

/* ========================================================================
 * Hash Table for GroupExpression Dedup
 * ======================================================================== */

static uint32
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

static int
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
     * Phase 6: When a duplicate expression is found in a DIFFERENT group
     * than its first occurrence, automatically merge the two groups.
     * This is the StarRocks "copyIn auto-merge" behavior:
     *   if the same (op + child groups) expression appears in two groups,
     *   those groups are semantically equivalent → merge them.
     */
    if ((int)expr->op < PG_CASCADES_PHYSICAL_SEQSCAN &&
        expr->mode != PG_PHYS_EXPR_IMPORTED_PATH &&
        memo->group_expr_table != NULL)
    {
        PgExprHashEntry *entry;
        bool             found;

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
     */
    if ((int)expr->op < PG_CASCADES_PHYSICAL_SEQSCAN &&
        expr->mode != PG_PHYS_EXPR_IMPORTED_PATH &&
        memo->group_expr_table != NULL)
    {
        PgExprHashEntry *entry;
        bool             dummy_found;

        entry = (PgExprHashEntry *)
            hash_search(memo->group_expr_table, &key, HASH_FIND, &dummy_found);
        if (entry != NULL && found)
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

    /* Set root_group if this is the top-level call */
    if (group != NULL)
        memo->root_group = group;

    return group;
}

/*
 * pg_memo_init:
 *   从 logical root expression 初始化 Memo。
 *   递归创建所有 Group 和 GroupExpression。
 */
PgMemo *
pg_memo_init(PgPlannerCascadesContext *ctx, PgGroupExpr *logical_root)
{
    PgMemo     *memo;
    MemoryContext old_cxt;
    HASHCTL     hash_ctl;

    old_cxt = MemoryContextSwitchTo(ctx->memo_cxt);

    memo = (PgMemo *) palloc0(sizeof(PgMemo));
    memo->context = ctx->memo_cxt;
    memo->groups = NIL;
    memo->root_group = NULL;

    /* Phase 4: Create hash table for GroupExpression dedup.
     * keysize = sizeof(PgExprHashKey) — only the key part is hashed/compared.
     * entrysize = sizeof(PgExprHashEntry) — stores key + owner_group_id
     *   for automatic group merging on duplicate detection. */
    MemSet(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(PgExprHashKey);
    hash_ctl.entrysize = sizeof(PgExprHashEntry);
    hash_ctl.hash = pg_memo_hash_key;
    hash_ctl.match = pg_memo_match_key;
    hash_ctl.hcxt = ctx->memo_cxt;
    memo->group_expr_table = hash_create("Memo GroupExpr Table", 256,
                                          &hash_ctl,
                                          HASH_ELEM | HASH_FUNCTION |
                                          HASH_COMPARE | HASH_CONTEXT);

    ctx->memo = memo;

    /* 递归构建 Memo */
    if (logical_root != NULL)
        memo->root_group = pg_memo_insert_expression(ctx, memo, logical_root, NULL);

    MemoryContextSwitchTo(old_cxt);
    return memo;
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

        /* Already derived? */
        if (group->logical_prop.relids != NULL)
            continue;

        /* Find the first logical expression to derive from */
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
                    if (ctx->upper != NULL)
                    {
                        if (list_length(expr->inputs) >= 1)
                        {
                            PgMemoGroup *child = (PgMemoGroup *) linitial(expr->inputs);
                            prop.relids = bms_copy(child->logical_prop.relids);
                        }
                        prop.rows = ctx->upper->dNumGroups;
                    }
                    break;

                default:
                    break;
            }

            /* Store derived property */
            group->logical_prop = prop;
            break;
        }
    }
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

    /* Clear source (expressions are now owned by target) */
    source->logical_exprs = NIL;
    source->physical_exprs = NIL;
    source->best_entries = NIL;

    if (ctx->debug)
        elog(NOTICE, "Cascades: merged group %d into group %d",
             source->id, target->id);
}
