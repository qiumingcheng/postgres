/*-------------------------------------------------------------------------
 * memo.c
 *    Cascades Memo 结构：Group / GroupExpression / 初始化 / 去重
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "utils/memutils.h"

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

    /*
     * If the expression was created by a rule transform, it should be
     * inserted into the same group as the source expression (parent_group).
     * If parent_group is provided, add to it; otherwise create a new group.
     */
    if (parent_group != NULL)
    {
        group = parent_group;

        /* Dedup: check if an equivalent logical expression already exists.
         * We compare op and input groups by pointer (PG's equal() doesn't
         * understand our PgMemoGroup type). */
        if ((int)expr->op < PG_CASCADES_PHYSICAL_SEQSCAN &&
            expr->mode != PG_PHYS_EXPR_IMPORTED_PATH)
        {
            ListCell *lc;
            foreach(lc, group->logical_exprs)
            {
                PgGroupExpr *existing = (PgGroupExpr *) lfirst(lc);
                if (existing->op == expr->op &&
                    list_length(existing->inputs) == list_length(expr->inputs))
                {
                    ListCell *a, *b;
                    bool same = true;
                    forboth(a, existing->inputs, b, expr->inputs)
                    {
                        if (lfirst(a) != lfirst(b))
                        {
                            same = false;
                            break;
                        }
                    }
                    if (same)
                        return NULL;  /* Duplicate — skip */
                }
            }
        }
    }
    else
    {
        group = pg_memo_new_group(ctx);
    }

    /* Add as logical (from pg_adapter) or physical (from rule transform) */
    if (expr->mode == PG_PHYS_EXPR_IMPORTED_PATH ||
        (int)expr->op >= PG_CASCADES_PHYSICAL_SEQSCAN)
        pg_memo_add_physical_expr(group, expr);
    else
        pg_memo_add_logical_expr(group, expr);

    /* inputs already contain PgMemoGroup * — no recursion needed */
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

    old_cxt = MemoryContextSwitchTo(ctx->memo_cxt);

    memo = (PgMemo *) palloc0(sizeof(PgMemo));
    memo->context = ctx->memo_cxt;
    memo->groups = NIL;
    memo->group_expr_table = NULL;
    memo->root_group = NULL;

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
