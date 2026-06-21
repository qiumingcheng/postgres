/*-------------------------------------------------------------------------
 * rule.c
 *    Cascades Rule 注册表：所有 implementation rule transform 函数
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"

/* ========================================================================
 * Phase 1 Implementation Rule Transform Functions (Upper Ops)
 * ======================================================================== */

static List *
pg_rule_agg_to_hashagg(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_HASHAGG);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = expr->inputs;
    result->op_private = ctx->upper;
    return list_make1(result);
}

static List *
pg_rule_agg_to_groupagg(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result;

    /* Only apply GroupAgg when GROUP BY is present */
    if (ctx->upper->groupClause == NIL)
        return NIL;

    result = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_PHYSICAL_GROUPAGG);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = expr->inputs;
    result->op_private = ctx->upper;
    return list_make1(result);
}

static List *
pg_rule_sort_to_physical_sort(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_SORT);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = expr->inputs;
    result->op_private = ctx->upper;
    return list_make1(result);
}

static List *
pg_rule_distinct_to_unique(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_UNIQUE);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = expr->inputs;
    result->op_private = ctx->upper;
    return list_make1(result);
}

static List *
pg_rule_limit_to_physical_limit(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_LIMIT);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = expr->inputs;
    result->op_private = ctx->upper;
    return list_make1(result);
}

static List *
pg_rule_project_to_physical_project(PgPlannerCascadesContext *ctx,
                                     PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_PROJECT);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = expr->inputs;
    result->op_private = ctx->upper;
    return list_make1(result);
}

/* ========================================================================
 * Phase 2 Implementation Rule Transform Functions (Scan/Join)
 * ======================================================================== */

static List *
pg_rule_scan_to_seqscan(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_SEQSCAN);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = NIL;
    result->op_private = expr->op_private;
    return list_make1(result);
}

static List *
pg_rule_scan_to_indexscan(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_INDEXSCAN);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = NIL;
    result->op_private = expr->op_private;
    return list_make1(result);
}

static List *
pg_rule_scan_to_bitmapheapscan(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = NIL;
    result->op_private = expr->op_private;
    return list_make1(result);
}

static List *
pg_rule_join_to_nestloop(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_NESTLOOP);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = expr->inputs;
    result->op_private = expr->op_private;
    return list_make1(result);
}

static List *
pg_rule_join_to_hashjoin(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_HASHJOIN);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = expr->inputs;
    result->op_private = expr->op_private;
    return list_make1(result);
}

static List *
pg_rule_join_to_mergejoin(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_MERGEJOIN);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = expr->inputs;
    result->op_private = expr->op_private;
    return list_make1(result);
}

/* ========================================================================
 * Transformation Rules (Phase 3)
 * ======================================================================== */

/*
 * pg_rule_join_commutativity:
 *   LogicalJoin(A, B) → LogicalJoin(B, A)
 */
static List *
pg_rule_join_commutativity(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *new_expr;
    PgMemoGroup *left_child;
    PgMemoGroup *right_child;

    if (list_length(expr->inputs) != 2)
        return NIL;

    left_child = (PgMemoGroup *) linitial(expr->inputs);
    right_child = (PgMemoGroup *) lsecond(expr->inputs);

    /* Don't swap if both children are the same (self-join handled elsewhere) */
    if (left_child == right_child)
        return NIL;

    new_expr = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    new_expr->inputs = list_make2(right_child, left_child);  /* swapped! */
    new_expr->op_private = expr->op_private;

    return list_make1(new_expr);
}

/* ========================================================================
 * Rule 注册表
 * ======================================================================== */

/* Phase 1: Upper ops (first phase enabled) */
static PgRule g_impl_rules_phase1[] = {
    {"LogicalAgg->PhysicalHashAgg", NULL, pg_rule_agg_to_hashagg,
     true, PG_CASCADES_LOGICAL_AGG, PG_CASCADES_PHYSICAL_HASHAGG},
    {"LogicalAgg->PhysicalGroupAgg", NULL, pg_rule_agg_to_groupagg,
     true, PG_CASCADES_LOGICAL_AGG, PG_CASCADES_PHYSICAL_GROUPAGG},
    {"LogicalSort->PhysicalSort", NULL, pg_rule_sort_to_physical_sort,
     true, PG_CASCADES_LOGICAL_SORT, PG_CASCADES_PHYSICAL_SORT},
    {"LogicalDistinct->PhysicalUnique", NULL, pg_rule_distinct_to_unique,
     true, PG_CASCADES_LOGICAL_DISTINCT, PG_CASCADES_PHYSICAL_UNIQUE},
    {"LogicalLimit->PhysicalLimit", NULL, pg_rule_limit_to_physical_limit,
     true, PG_CASCADES_LOGICAL_LIMIT, PG_CASCADES_PHYSICAL_LIMIT},
    {"LogicalProject->PhysicalProject", NULL, pg_rule_project_to_physical_project,
     true, PG_CASCADES_LOGICAL_PROJECT, PG_CASCADES_PHYSICAL_PROJECT},
    {NULL, NULL, NULL, false, 0, 0}  /* sentinel */
};

/* Phase 3: Transformation rules */
static PgRule g_trans_rules_phase3[] = {
    {"JoinCommutativity", NULL, pg_rule_join_commutativity,
     false, PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_LOGICAL_JOIN},
    {NULL, NULL, NULL, false, 0, 0}  /* sentinel */
};

/* Phase 2: Scan/Join (second phase enabled) */
static PgRule g_impl_rules_phase2[] = {
    {"LogicalScan->PhysicalSeqScan", NULL, pg_rule_scan_to_seqscan,
     true, PG_CASCADES_LOGICAL_SCAN, PG_CASCADES_PHYSICAL_SEQSCAN},
    {"LogicalScan->PhysicalIndexScan", NULL, pg_rule_scan_to_indexscan,
     true, PG_CASCADES_LOGICAL_SCAN, PG_CASCADES_PHYSICAL_INDEXSCAN},
    {"LogicalScan->PhysicalBitmapHeapScan", NULL, pg_rule_scan_to_bitmapheapscan,
     true, PG_CASCADES_LOGICAL_SCAN, PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN},
    {"LogicalJoin->PhysicalNestLoop", NULL, pg_rule_join_to_nestloop,
     true, PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_NESTLOOP},
    {"LogicalJoin->PhysicalHashJoin", NULL, pg_rule_join_to_hashjoin,
     true, PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_HASHJOIN},
    {"LogicalJoin->PhysicalMergeJoin", NULL, pg_rule_join_to_mergejoin,
     true, PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_MERGEJOIN},
    {NULL, NULL, NULL, false, 0, 0}  /* sentinel */
};

/* ========================================================================
 * 公开接口
 * ======================================================================== */

PgRule *
pg_cascades_get_impl_rules(int *num_rules)
{
    int i = 0;
    while (g_impl_rules_phase1[i].name != NULL)
        i++;
    *num_rules = i;
    return g_impl_rules_phase1;
}

PgRule *
pg_cascades_get_trans_rules(int *num_rules)
{
    int i = 0;
    while (g_trans_rules_phase3[i].name != NULL)
        i++;
    *num_rules = i;
    return g_trans_rules_phase3;
}

PgRule *
pg_cascades_get_impl_rules_phase2(int *num_rules)
{
    int i = 0;
    while (g_impl_rules_phase2[i].name != NULL)
        i++;
    *num_rules = i;
    return g_impl_rules_phase2;
}
