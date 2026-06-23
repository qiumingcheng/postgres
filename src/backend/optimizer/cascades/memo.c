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
     * Phase 4: Global hash table dedup for logical expressions.
     * Check if an equivalent expression already exists in any group.
     */
    if ((int)expr->op < PG_CASCADES_PHYSICAL_SEQSCAN &&
        expr->mode != PG_PHYS_EXPR_IMPORTED_PATH &&
        memo->group_expr_table != NULL)
    {
        (void) hash_search(memo->group_expr_table, &key, HASH_ENTER, &found);
        if (found)
            return NULL;  /* Duplicate — skip */
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

    /* Add as logical or physical */
    if (expr->mode == PG_PHYS_EXPR_IMPORTED_PATH ||
        (int)expr->op >= PG_CASCADES_PHYSICAL_SEQSCAN)
        pg_memo_add_physical_expr(group, expr);
    else
        pg_memo_add_logical_expr(group, expr);

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

    /* Phase 4: Create hash table for GroupExpression dedup */
    MemSet(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(PgExprHashKey);
    hash_ctl.entrysize = sizeof(PgExprHashKey);
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
