/*-------------------------------------------------------------------------
 * rule.c
 *    Cascades Rule 注册表：所有 implementation rule transform 函数
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/cost.h"
#include "utils/memutils.h"

/* ========================================================================
 * Phase 1 Implementation Rule Transform Functions (Upper Ops)
 * ======================================================================== */

List *
pg_rule_agg_to_hashagg(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_HASHAGG);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = expr->inputs;
    result->op_private = ctx->upper;
    return list_make1(result);
}

List *
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

List *
pg_rule_sort_to_physical_sort(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_SORT);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = expr->inputs;
    result->op_private = ctx->upper;
    return list_make1(result);
}

List *
pg_rule_distinct_to_unique(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_UNIQUE);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = expr->inputs;
    result->op_private = ctx->upper;
    return list_make1(result);
}

List *
pg_rule_limit_to_physical_limit(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_LIMIT);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = expr->inputs;
    result->op_private = ctx->upper;
    return list_make1(result);
}

List *
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

List *
pg_rule_scan_to_seqscan(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_SEQSCAN);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = NIL;
    result->op_private = expr->op_private;
    return list_make1(result);
}

List *
pg_rule_scan_to_indexscan(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_INDEXSCAN);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = NIL;
    result->op_private = expr->op_private;
    return list_make1(result);
}

List *
pg_rule_scan_to_bitmapheapscan(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = NIL;
    result->op_private = expr->op_private;
    return list_make1(result);
}

List *
pg_rule_join_to_nestloop(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *outer_grp, *inner_grp;

    outer_grp = (PgMemoGroup *) linitial(expr->inputs);
    inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

    /* Safety: skip if either child is empty (pruned by rewrite) */
    if (outer_grp == NULL || inner_grp == NULL)
        return NIL;
    if ((outer_grp->rows == 0 && outer_grp->width == 0) ||
        (inner_grp->rows == 0 && inner_grp->width == 0))
        return NIL;

    /*
     * Always create COMPOSABLE_OP NESTLOOP — the cost function
     * (cost_nestloop) adds disable_cost when enable_nestloop=off,
     * making it a last resort that's only chosen when no other join
     * method is viable (e.g., cross joins).  This matches PG's
     * behavior: NestPaths are always generated, never disabled.
     */
    {
        PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                      PG_CASCADES_PHYSICAL_NESTLOOP);
        result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
        result->inputs = expr->inputs;
        result->op_private = expr->op_private;
        return list_make1(result);
    }
}

List *
pg_rule_join_to_hashjoin(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *outer_grp, *inner_grp;

    outer_grp = (PgMemoGroup *) linitial(expr->inputs);
    inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

    if (outer_grp == NULL || inner_grp == NULL)
        return NIL;
    if ((outer_grp->rows == 0 && outer_grp->width == 0) ||
        (inner_grp->rows == 0 && inner_grp->width == 0))
        return NIL;

    if (!enable_hashjoin)
        return NIL;

    {
        PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                      PG_CASCADES_PHYSICAL_HASHJOIN);
        result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
        result->inputs = expr->inputs;
        result->op_private = expr->op_private;
        return list_make1(result);
    }
}

List *
pg_rule_join_to_mergejoin(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *outer_grp, *inner_grp;

    outer_grp = (PgMemoGroup *) linitial(expr->inputs);
    inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

    if (outer_grp == NULL || inner_grp == NULL)
        return NIL;
    if ((outer_grp->rows == 0 && outer_grp->width == 0) ||
        (inner_grp->rows == 0 && inner_grp->width == 0))
        return NIL;

    if (!enable_mergejoin)
        return NIL;

    {
        PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                      PG_CASCADES_PHYSICAL_MERGEJOIN);
        result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
        result->inputs = expr->inputs;
        result->op_private = expr->op_private;
        return list_make1(result);
    }
}

/* ========================================================================
 * Transformation Rules (Phase 3)
 * ======================================================================== */

/*
 * pg_rule_join_commutativity:
 *   LogicalJoin(A, B) → LogicalJoin(B, A)
 */
List *
pg_rule_join_commutativity(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *new_expr;
    PgMemoGroup *left_child;
    PgMemoGroup *right_child;

    left_child = (PgMemoGroup *) linitial(expr->inputs);
    right_child = (PgMemoGroup *) lsecond(expr->inputs);

    if (left_child == NULL || right_child == NULL)
        return NIL;

    /* Don't swap if both children are the same (self-join handled elsewhere) */
    if (left_child == right_child)
        return NIL;

    new_expr = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    new_expr->inputs = list_make2(right_child, left_child);  /* swapped! */
    new_expr->op_private = expr->op_private;

    return list_make1(new_expr);
}

/* ========================================================================
 * Rule 注册表 (Phase 5: 使用 rule_type + pattern 替代 is_implementation)
 * ======================================================================== */

/* Phase 1: Upper ops (first phase enabled) */
static PgRule g_impl_rules_phase1[] = {
    {"LogicalAgg->PhysicalHashAgg", NULL, pg_rule_agg_to_hashagg,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_AGG, PG_CASCADES_PHYSICAL_HASHAGG,
     PG_RULE_BIT_AGG_TO_HASHAGG, 0.8},
    {"LogicalAgg->PhysicalGroupAgg", NULL, pg_rule_agg_to_groupagg,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_AGG, PG_CASCADES_PHYSICAL_GROUPAGG,
     PG_RULE_BIT_AGG_TO_GROUPAGG, 0.7},
    {"LogicalSort->PhysicalSort", NULL, pg_rule_sort_to_physical_sort,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_SORT, PG_CASCADES_PHYSICAL_SORT,
     PG_RULE_BIT_SORT_TO_SORT, 1.0},
    {"LogicalDistinct->PhysicalUnique", NULL, pg_rule_distinct_to_unique,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_DISTINCT, PG_CASCADES_PHYSICAL_UNIQUE,
     PG_RULE_BIT_DISTINCT_TO_UNIQUE, 1.0},
    {"LogicalLimit->PhysicalLimit", NULL, pg_rule_limit_to_physical_limit,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_LIMIT, PG_CASCADES_PHYSICAL_LIMIT,
     PG_RULE_BIT_LIMIT_TO_LIMIT, 1.0},
    {"LogicalProject->PhysicalProject", NULL, pg_rule_project_to_physical_project,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_PROJECT, PG_CASCADES_PHYSICAL_PROJECT,
     PG_RULE_BIT_PROJECT_TO_PROJECT, 1.0},
    {NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0.0}  /* sentinel */
};

/* Phase 3: Transformation rules */
static PgRule g_trans_rules_phase3[] = {
    {"JoinCommutativity", NULL, pg_rule_join_commutativity,
     PG_RULE_TRANS, NULL, PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_LOGICAL_JOIN,
     PG_RULE_BIT_JOIN_COMMUTATIVITY, 0.5},
    {NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0.0}  /* sentinel */
};

/* Phase 4: Forward declarations for path-generation join rules */
static List *pg_rule_join_to_hashjoin_phase4(PgPlannerCascadesContext *ctx,
                                              PgGroupExpr *expr);
static List *pg_rule_join_to_nestloop_phase4(PgPlannerCascadesContext *ctx,
                                              PgGroupExpr *expr);
static List *pg_rule_join_to_mergejoin_phase4(PgPlannerCascadesContext *ctx,
                                               PgGroupExpr *expr);

/*
 * Phase 2: Scan-only implementation rules (safe to wire now).
 * These match LogicalScan nodes created by transformation rules
 * (e.g. A1 PushDownPredicateScan) and produce COMPOSABLE_OP physical
 * expressions with NIL inputs (no child groups, safe in Path-import mode).
 */
static PgRule g_impl_rules_phase2_scan[] = {
    {"LogicalScan->PhysicalSeqScan", NULL, pg_rule_scan_to_seqscan,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_SCAN, PG_CASCADES_PHYSICAL_SEQSCAN,
     PG_RULE_BIT_SCAN_TO_SEQSCAN, 0.5},
    {"LogicalScan->PhysicalIndexScan", NULL, pg_rule_scan_to_indexscan,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_SCAN, PG_CASCADES_PHYSICAL_INDEXSCAN,
     PG_RULE_BIT_SCAN_TO_INDEXSCAN, 0.8},
    {"LogicalScan->PhysicalBitmapHeapScan", NULL, pg_rule_scan_to_bitmapheapscan,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_SCAN, PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN,
     PG_RULE_BIT_SCAN_TO_BITMAPSCAN, 0.7},
    {NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0.0}  /* sentinel */
};

/* Phase 2: Full Scan/Join rules (for Phase 6 Path-generation mode) */
static PgRule g_impl_rules_phase2[] = {
    {"LogicalScan->PhysicalSeqScan", NULL, pg_rule_scan_to_seqscan,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_SCAN, PG_CASCADES_PHYSICAL_SEQSCAN,
     PG_RULE_BIT_SCAN_TO_SEQSCAN, 0.5},
    {"LogicalScan->PhysicalIndexScan", NULL, pg_rule_scan_to_indexscan,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_SCAN, PG_CASCADES_PHYSICAL_INDEXSCAN,
     PG_RULE_BIT_SCAN_TO_INDEXSCAN, 0.8},
    {"LogicalScan->PhysicalBitmapHeapScan", NULL, pg_rule_scan_to_bitmapheapscan,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_SCAN, PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN,
     PG_RULE_BIT_SCAN_TO_BITMAPSCAN, 0.7},
    {"LogicalJoin->PhysicalNestLoop", NULL, pg_rule_join_to_nestloop,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_NESTLOOP,
     PG_RULE_BIT_JOIN_TO_NESTLOOP, 0.5},
    {"LogicalJoin->PhysicalHashJoin", NULL, pg_rule_join_to_hashjoin,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_HASHJOIN,
     PG_RULE_BIT_JOIN_TO_HASHJOIN, 0.8},
    {"LogicalJoin->PhysicalMergeJoin", NULL, pg_rule_join_to_mergejoin,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_MERGEJOIN,
     PG_RULE_BIT_JOIN_TO_MERGEJOIN, 0.6},
    /* Phase 4: Path-generation join rules (call make_join_rel internally) */
    {"LogicalJoin->PhysicalHashJoin_Phase4", NULL,
     pg_rule_join_to_hashjoin_phase4,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_HASHJOIN,
     PG_RULE_BIT_JOIN_TO_HASHJOIN_PHASE4, 0.9},
    {"LogicalJoin->PhysicalNestLoop_Phase4", NULL,
     pg_rule_join_to_nestloop_phase4,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_NESTLOOP,
     PG_RULE_BIT_JOIN_TO_NESTLOOP_PHASE4, 0.9},
    {"LogicalJoin->PhysicalMergeJoin_Phase4", NULL,
     pg_rule_join_to_mergejoin_phase4,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_MERGEJOIN,
     PG_RULE_BIT_JOIN_TO_MERGEJOIN_PHASE4, 0.9},
    {NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0.0}  /* sentinel */
};

/* Phase 2: Join-only rules (safe for tree-based Memo) */
static PgRule g_impl_rules_phase2_join[] = {
    {"LogicalJoin->PhysicalNestLoop", NULL, pg_rule_join_to_nestloop,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_NESTLOOP,
     PG_RULE_BIT_JOIN_TO_NESTLOOP, 0.5},
    {"LogicalJoin->PhysicalHashJoin", NULL, pg_rule_join_to_hashjoin,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_HASHJOIN,
     PG_RULE_BIT_JOIN_TO_HASHJOIN, 0.8},
    {"LogicalJoin->PhysicalMergeJoin", NULL, pg_rule_join_to_mergejoin,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_MERGEJOIN,
     PG_RULE_BIT_JOIN_TO_MERGEJOIN, 0.6},
    {NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0.0}
};


/* ========================================================================
 * Phase 5: Transformation rule transform functions
 *
 *   Phase 6 note: In the Memo, expr->inputs are PgMemoGroup*, NOT
 *   PgGroupExpr*.  Rules that need to access child expression fields
 *   must use pg_memo_group_first_logical() to look up the first
 *   logical expression of the expected op kind in the child group.
 * ======================================================================== */

/*
 * pg_memo_group_first_logical:
 *   Helper: find the first logical expression of a given op kind
 *   in a Memo group.  Returns NULL if none found.
 */
static PgGroupExpr *
pg_memo_group_first_logical(PgMemoGroup *group, PgCascadesOpKind op)
{
    ListCell *lc;

    if (group == NULL)
        return NULL;

    foreach(lc, group->logical_exprs)
    {
        PgGroupExpr *e = (PgGroupExpr *) lfirst(lc);
        if (e->op == op)
            return e;
    }
    return NULL;
}

/*
 * H2: MergeProjectWithChild
 *   LogicalProject(LogicalProject(A)) → LogicalProject(A)
 *   合并两个连续的 Project：内层 tlist 被外层 tlist 替代。
 *
 *   Phase 6: inputs are PgMemoGroup* (Memo groups), not PgGroupExpr*.
 *   Look up the first LogicalProject in the child group to examine.
 */
static List *
pg_rule_merge_project_with_child(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *child_group;
    PgGroupExpr *inner = NULL;
    PgGroupExpr *new_proj;
    ListCell   *lc;

    if (expr->inputs == NIL)
        return NIL;

    child_group = (PgMemoGroup *) linitial(expr->inputs);
    if (child_group == NULL || child_group->logical_exprs == NIL)
        return NIL;

    /* Find LogicalProject in child group's logical expressions */
    foreach(lc, child_group->logical_exprs)
    {
        PgGroupExpr *e = (PgGroupExpr *) lfirst(lc);
        if (e->op == PG_CASCADES_LOGICAL_PROJECT)
        {
            inner = e;
            break;
        }
    }

    if (inner == NULL)
        return NIL;

    /* 新 Project 直接用外层的 tlist，child 指向内层的 child */
    new_proj = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_PROJECT);
    new_proj->inputs = inner->inputs;       /* 跳过内层 Project */
    new_proj->op_private = expr->op_private; /* 外层 tlist */
    return list_make1(new_proj);
}

/*
 * E2: PruneEmptyScan
 *   当 scan 的 RelOptInfo->rows == 0 时，标记 group 为空。
 *   Side-effect-only：不生成新 expression，只更新 group->rows=0。
 */
static List *
pg_rule_prune_empty_scan(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    RelOptInfo *rel = expr->owner_group->rel;

    if (rel == NULL || rel->rows > 0)
        return NIL;

    /* 标记 group 为空（让后续 E1 PruneEmptyJoin 能检测到） */
    expr->owner_group->rows = 0;
    expr->owner_group->width = 0;
    return NIL;
}

/*
 * H3: EliminateProject
 *   当 Project 的 tlist 和 child tlist 完全相同时，删除冗余 Project。
 *
 *   Phase 6: inputs are PgMemoGroup*, look up LogicalProject in child group.
 */
static List *
pg_rule_eliminate_project(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *child_group;
    PgGroupExpr *child = NULL;
    ListCell   *lc;

    if (expr->inputs == NIL)
        return NIL;

    child_group = (PgMemoGroup *) linitial(expr->inputs);

    /* Find LogicalProject in child group's logical expressions */
    foreach(lc, child_group->logical_exprs)
    {
        PgGroupExpr *e = (PgGroupExpr *) lfirst(lc);
        if (e->op == PG_CASCADES_LOGICAL_PROJECT)
        {
            child = e;
            break;
        }
    }

    /* 如果 child 不是 Project，无法判断 tlist 是否相同 —— 保守保留 */
    if (child == NULL)
        return NIL;

    /*
     * 简单启发式：如果外层 tlist 和内层 tlist 指针相同（由 pg_adapter 保证），
     * 则消除外层 Project。
     * 更精确的检查需要逐项比较 TargetEntry，第一版用指针相等。
     */
    if (expr->op_private == child->op_private)
    {
        /* 创建新的 expression，跳过当前 Project */
        PgGroupExpr *new_expr = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_PROJECT);
        new_expr->inputs = child->inputs;
        new_expr->op_private = child->op_private;
        return list_make1(new_expr);
    }

    return NIL;
}

/*
 * A1: PushDownPredicateScan
 *   LogicalFilter(LogicalScan) → LogicalScan (filter 融入 baserestrictinfo)
 *
 *   Phase 6: inputs are PgMemoGroup*, not PgGroupExpr*.
 *   Use child_group->rel directly since tree-based LogicalScan groups
 *   have rel set by pg_memo_insert_expression_tree.
 */
static List *
pg_rule_pushdown_predicate_scan(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *child_group;
    List        *filter_quals = (List *) expr->op_private;
    RelOptInfo  *rel;
    ListCell    *lc;
    List        *pushable = NIL;
    List        *remain   = NIL;

    if (expr->inputs == NIL)
        return NIL;

    child_group = (PgMemoGroup *) linitial(expr->inputs);
    rel = child_group->rel;

    if (rel == NULL || filter_quals == NIL)
        return NIL;

    foreach(lc, filter_quals)
    {
        RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

        /* volatile → 不推 */
        if (contain_volatile_functions((Node *) ri->clause))
        {
            remain = lappend(remain, ri);
            continue;
        }

        /* qual 涉及 scan 之外的 rel → 不推 */
        if (!bms_is_subset(ri->clause_relids, rel->relids))
        {
            remain = lappend(remain, ri);
            continue;
        }

        pushable = lappend(pushable, ri);
    }

    if (pushable == NIL)
        return NIL;

    /* 将可推 qual 合并到 baserestrictinfo */
    rel->baserestrictinfo = list_concat(rel->baserestrictinfo, pushable);

    if (remain == NIL)
    {
        /* 所有 qual 都推完了 → 返回裸 LogicalScan */
        PgGroupExpr *scan_logical = pg_memo_group_first_logical(child_group,
                                        PG_CASCADES_LOGICAL_SCAN);
        PgGroupExpr *new_scan = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_SCAN);
        new_scan->inputs = NIL;
        new_scan->op_private = (scan_logical != NULL) ?
            scan_logical->op_private : NULL;
        return list_make1(new_scan);
    }
    else
    {
        /* 还有 qual → 返回变薄的 LogicalFilter */
        PgGroupExpr *new_filter = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_FILTER);
        new_filter->inputs = expr->inputs;  /* child 不变 */
        new_filter->op_private = remain;
        return list_make1(new_filter);
    }
}

/*
 * D1: MergeLimitWithSort
 *   LogicalLimit(LogicalSort(A)) → LogicalSort(A) with limit_tuples set
 *   消除冗余的 Limit 节点，将 limit_tuples 记录到 Sort 的 PgSortPrivate 中。
 */
List *
pg_rule_merge_limit_with_sort(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *sort_expr;
    PgSortPrivate *sort_priv;
    PgGroupExpr *new_sort;
    double limit_tuples;

    sort_expr = pg_memo_group_first_logical(
        (PgMemoGroup *) linitial(expr->inputs), PG_CASCADES_LOGICAL_SORT);
    if (sort_expr == NULL)
        return NIL;

    limit_tuples = ctx->upper->limit_tuples;
    if (limit_tuples <= 0)
        return NIL;  /* 无有效 LIMIT */

    /* 创建新的 LogicalSort，将 limit_tuples 注入 PgSortPrivate */
    new_sort = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_SORT);
    new_sort->inputs = sort_expr->inputs;  /* child 透传 */

    sort_priv = (PgSortPrivate *) palloc(sizeof(PgSortPrivate));
    sort_priv->pathkeys = (List *) sort_expr->op_private;
    sort_priv->limit_tuples = limit_tuples;
    new_sort->op_private = sort_priv;

    return list_make1(new_sort);
}

/*
 * H1: EliminateSortWithConstantKey
 *   当 Sort 的所有 pathkeys 都来自常量表达式时，消除无效 Sort。
 */
static List *
pg_rule_eliminate_sort_with_constant_key(PgPlannerCascadesContext *ctx,
                                          PgGroupExpr *expr)
{
    /*
     * 第一版简化：如果 query_pathkeys == NIL（无实际排序要求），
     * 消除 Sort 节点。更精确的检查需要遍历 PathKey 判断是否常量。
     */
    if (ctx->upper->sort_pathkeys == NIL &&
        ctx->upper->group_pathkeys == NIL)
    {
        /* 无排序要求 → 跳过 Sort，直接透传 child */
        PgGroupExpr *new_child = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_PROJECT);
        new_child->inputs = expr->inputs;  /* child 透传 */
        new_child->op_private = ctx->upper->tlist;
        return list_make1(new_child);
    }

    return NIL;
}

/*
 * H4: MergeFilterWithJoin
 *   LogicalJoin(LogicalFilter(A), B) → LogicalJoin(A, B)
 *   将一侧的 Filter 条件合并到 Join 的 restrictlist 中。
 *   仅对 INNER JOIN 生效。
 */
static List *
pg_rule_merge_filter_with_join(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *outer_input;
    PgGroupExpr *new_join;
    PgJoinPrivate *join_priv;

    join_priv = (PgJoinPrivate *) expr->op_private;
    if (join_priv == NULL || join_priv->jointype != JOIN_INNER)
        return NIL;

    outer_input = pg_memo_group_first_logical(
        (PgMemoGroup *) linitial(expr->inputs), PG_CASCADES_LOGICAL_FILTER);

    /* 只处理 outer 侧有 Filter 的情况 */
    if (outer_input == NULL)
        return NIL;

    {
        List *extra_quals = (List *) outer_input->op_private;
        ListCell *lc;

        /* 检查是否有 volatile function — 有则不合并 */
        foreach(lc, extra_quals)
        {
            RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
            if (contain_volatile_functions((Node *) ri->clause))
                return NIL;
        }

        /* 将 filter quals 合并到 join restrictlist */
        join_priv->restrictlist = list_concat(join_priv->restrictlist,
                                               extra_quals);
    }

    /* 构建新 LogicalJoin：child 改为 Filter 的 child（跳过 Filter） */
    new_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    new_join->inputs = list_make2(linitial(outer_input->inputs),  /* Filter 的 child */
                                   lsecond(expr->inputs));        /* inner 不变 */
    new_join->op_private = join_priv;

    return list_make1(new_join);
}

/*
 * E3: PruneEmptyUnion
 *   LogicalUnion(A, B) where A 的 group->rows == 0 → 消除空分支。
 */
static List *
pg_rule_prune_empty_union(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    ListCell   *lc;
    List       *non_empty = NIL;
    bool        any_empty = false;

    foreach(lc, expr->inputs)
    {
        PgMemoGroup *child = (PgMemoGroup *) lfirst(lc);

        if (child->rows == 0 && child->width == 0)
        {
            any_empty = true;
            continue;  /* 跳过空分支 */
        }
        non_empty = lappend(non_empty, child);
    }

    if (!any_empty)
        return NIL;

    if (list_length(non_empty) == 0)
    {
        /* 所有分支都空 → 标记当前 group 为空 */
        expr->owner_group->rows = 0;
        expr->owner_group->width = 0;
        return NIL;
    }

    if (list_length(non_empty) == 1)
    {
        /* 只剩一个分支 → 创建透传 expression */
        PgGroupExpr *new_expr = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_PROJECT);
        new_expr->inputs = non_empty;
        new_expr->op_private = ctx->upper->tlist;
        return list_make1(new_expr);
    }

    /* 多个非空分支 → 重建 Union */
    {
        PgGroupExpr *new_union = pg_memo_new_group_expr(ctx,
                                        PG_CASCADES_LOGICAL_PROJECT);
        new_union->inputs = non_empty;
        new_union->op_private = expr->op_private;
        return list_make1(new_union);
    }
}

/*
 * F2: MergeTwoAgg
 *   LogicalAgg(LogicalAgg(A)) → LogicalAgg(A)
 *   合并两层聚合：如果内层 Agg 和外层 Agg 的 groupClause 相同，合并。
 */
static List *
pg_rule_merge_two_agg(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *inner;

    inner = pg_memo_group_first_logical(
        (PgMemoGroup *) linitial(expr->inputs), PG_CASCADES_LOGICAL_AGG);
    if (inner == NULL)
        return NIL;

    /*
     * 第一版简化：仅当内外 Agg 的 op_private 指针相同时合并
     * （在 pg_adapter 构建时，同一查询的 Agg 节点共享 PgCascadesUpperInfo）。
     */
    if (expr->op_private != inner->op_private)
        return NIL;

    /* 跳过内层 Agg，直接引用内层的 child */
    {
        PgGroupExpr *new_agg = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_AGG);
        new_agg->inputs = inner->inputs;
        new_agg->op_private = expr->op_private;
        return list_make1(new_agg);
    }
}

/*
 * G4: MergeJoinWithChildProject
 *   LogicalJoin(LogicalProject(A), LogicalProject(B))
 *   → LogicalJoin(A, B) when Projects are trivial passthrough.
 */
static List *
pg_rule_merge_join_with_child_project(PgPlannerCascadesContext *ctx,
                                       PgGroupExpr *expr)
{
    PgGroupExpr *outer_input, *inner_input;
    bool outer_is_proj, inner_is_proj;
    PgGroupExpr *new_join;

    outer_input = pg_memo_group_first_logical(
        (PgMemoGroup *) linitial(expr->inputs), PG_CASCADES_LOGICAL_PROJECT);
    inner_input = pg_memo_group_first_logical(
        (PgMemoGroup *) lsecond(expr->inputs), PG_CASCADES_LOGICAL_PROJECT);

    outer_is_proj = (outer_input != NULL);
    inner_is_proj = (inner_input != NULL);

    if (!outer_is_proj && !inner_is_proj)
        return NIL;

    new_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    new_join->inputs = list_make2(
        outer_is_proj ? linitial(outer_input->inputs) : (void *) outer_input,
        inner_is_proj ? linitial(inner_input->inputs) : (void *) inner_input);
    new_join->op_private = expr->op_private;

    return list_make1(new_join);
}

/* ========================================================================
 * Phase 5b: Column Pruning Rules (B group)
 *   These rules reduce column sets to improve I/O efficiency.
 *   First version: side-effect — modify RelOptInfo->reltargetlist.
 * ======================================================================== */

/*
 * B3: PruneAggColumns
 *   LogicalAgg(A): keep only GROUP BY columns + Aggref argument columns.
 *   Sets group->logical_prop.output_columns to the restricted column set.
 *   B1 (PruneScanColumns) reads this to prune reltargetlist lower down.
 */
static List *
pg_rule_prune_agg_columns(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgCascadesUpperInfo *upper = ctx->upper;
    Bitmapset  *needed = NULL;
    ListCell   *lc;

    if (upper == NULL)
        return NIL;

    /* Collect GROUP BY columns */
    if (upper->groupClause != NIL)
    {
        foreach(lc, upper->groupClause)
        {
            SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
            TargetEntry *te = get_sortgroupclause_tle(sgc, upper->tlist);
            if (te != NULL)
                needed = bms_add_member(needed, te->resno);
        }
    }

    /* Collect aggref argument columns from target list */
    foreach(lc, upper->tlist)
    {
        TargetEntry *te = (TargetEntry *) lfirst(lc);
        if (IsA(te->expr, Aggref))
        {
            Aggref *agg = (Aggref *) te->expr;
            ListCell *alc;
            foreach(alc, agg->args)
            {
                Node *arg = (Node *) lfirst(alc);
                if (IsA(arg, Var))
                    needed = bms_add_member(needed, ((Var *) arg)->varattno);
            }
        }
    }

    if (needed != NULL)
    {
        /* Prune: union with existing output_columns so we don't lose parent needs */
        if (expr->owner_group->logical_prop.output_columns != NULL)
            needed = bms_union(needed,
                        expr->owner_group->logical_prop.output_columns);
        expr->owner_group->logical_prop.output_columns = needed;
    }
    return NIL;
}

/*
 * pg_collect_all_var_attnos:
 *   Walk an expression tree and collect ALL Var->varattno values
 *   into a Bitmapset, regardless of varno.  Unlike PG's pull_varattnos()
 *   which filters by a specific varno, this captures every Var reference
 *   in the tree — needed for B4's column propagation where child columns
 *   from multiple tables must all be preserved.
 */
static bool
pg_collect_var_attnos_walker(Node *node, void *context)
{
    Bitmapset **needed = (Bitmapset **) context;

    if (node == NULL)
        return false;
    if (IsA(node, Var))
    {
        Var *var = (Var *) node;
        *needed = bms_add_member(*needed, var->varattno);
        return false;  /* don't recurse into Var */
    }
    return expression_tree_walker(node, pg_collect_var_attnos_walker, context);
}

static void
pg_collect_all_var_attnos(Node *expr, Bitmapset **needed)
{
    pg_collect_var_attnos_walker(expr, needed);
}

/*
 * B4: PruneProjectColumns
 *   LogicalProject(A): keep only columns referenced by parent.
 *   Reads parent's output_columns from group->logical_prop,
 *   maps them through the project target list to derive what
 *   the child needs, and sets the child group's output_columns.
 */
static List *
pg_rule_prune_project_columns(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgCascadesUpperInfo *upper = ctx->upper;
    Bitmapset  *needed = NULL;
    ListCell   *lc;

    if (upper == NULL || upper->tlist == NIL)
        return NIL;

    /* Start from parent-required columns (from logical_prop) */
    if (expr->owner_group->logical_prop.output_columns != NULL)
        needed = bms_copy(expr->owner_group->logical_prop.output_columns);

    /* Map each target entry: if parent needs resno X, find what child vars
     * contribute to that expression.  First version: walk tlist entries
     * and pull all Var references whose resno is in needed. */
    foreach(lc, upper->tlist)
    {
        TargetEntry *te = (TargetEntry *) lfirst(lc);
        if (needed != NULL && !bms_is_member(te->resno, needed))
            continue;
        /* Include all Var references used in this target entry */
        pg_collect_all_var_attnos((Node *) te->expr, &needed);
    }

    /* Propagate needed columns to child group */
    if (needed != NULL && list_length(expr->inputs) >= 1)
    {
        PgMemoGroup *child = (PgMemoGroup *) linitial(expr->inputs);
        if (child->logical_prop.output_columns != NULL)
            needed = bms_union(needed, child->logical_prop.output_columns);
        child->logical_prop.output_columns = needed;
    }
    else if (needed != NULL)
        bms_free(needed);

    return NIL;
}

/*
 * B5: PruneSortColumns
 *   LogicalSort(A): keep only sort key columns + parent required columns.
 *   Sets group->logical_prop.output_columns to the restricted set.
 */
static List *
pg_rule_prune_sort_columns(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    Bitmapset  *needed = NULL;
    ListCell   *lc;

    /* Collect columns from sort pathkeys */
    foreach(lc, ctx->upper->sort_pathkeys)
    {
        PathKey *pk = (PathKey *) lfirst(lc);
        ListCell *ec;
        foreach(ec, pk->pk_eclass->ec_members)
        {
            EquivalenceMember *em = (EquivalenceMember *) lfirst(ec);
            if (IsA(em->em_expr, Var))
            {
                Var *v = (Var *) em->em_expr;
                needed = bms_add_member(needed, v->varattno);
            }
        }
    }

    if (needed != NULL)
    {
        /* Union with parent-required output_columns */
        if (expr->owner_group->logical_prop.output_columns != NULL)
            needed = bms_union(needed,
                        expr->owner_group->logical_prop.output_columns);
        expr->owner_group->logical_prop.output_columns = needed;
    }
    return NIL;
}

/*
 * E1: PruneEmptyJoin
 *   LogicalJoin(A, B) where A->rows==0 or B->rows==0 → mark group as empty.
 *   Side-effect: sets group->rows=0 so upper nodes can detect empty sets.
 */
static List *
pg_rule_prune_empty_join(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *outer_grp, *inner_grp;

    outer_grp = (PgMemoGroup *) linitial(expr->inputs);
    inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

    if ((outer_grp->rows == 0 && outer_grp->width == 0) ||
        (inner_grp->rows == 0 && inner_grp->width == 0))
    {
        expr->owner_group->rows = 0;
        expr->owner_group->width = 0;
    }

    return NIL;
}

/*
 * B1: PruneScanColumns
 *   LogicalScan: reduce reltargetlist to columns needed by parent.
 *   Uses baserestrictinfo + logical_prop.output_columns from parent
 *   propagation (B2-B5) + best_entries' required_columns.
 */
static List *
pg_rule_prune_scan_columns(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    RelOptInfo *rel = expr->owner_group->rel;
    ListCell   *lc;
    Bitmapset  *needed = NULL;
    List       *new_tlist = NIL;
    bool        pruned = false;

    if (rel == NULL)
        return NIL;

    /* Collect columns from baserestrictinfo (quals reference these) */
    foreach(lc, rel->baserestrictinfo)
    {
        RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
        pull_varattnos((Node *) ri->clause, rel->relid, &needed);
    }

    /* Always keep columns referenced by the target list (upper->tlist) */
    if (ctx->upper != NULL && ctx->upper->tlist != NIL)
    {
        pull_varattnos((Node *) ctx->upper->tlist, rel->relid, &needed);
    }

    /* Also collect from logical_prop.output_columns (B2-B5 propagation) */
    if (expr->owner_group->logical_prop.output_columns != NULL)
    {
        needed = bms_union(needed,
                    bms_copy(expr->owner_group->logical_prop.output_columns));
    }

    /* Also collect from best entries' required_columns (parent projections) */
    foreach(lc, expr->owner_group->best_entries)
    {
        PgGroupBestEntry *entry = (PgGroupBestEntry *) lfirst(lc);
        if (entry->required != NULL && entry->required->required_columns != NULL)
        {
            needed = bms_union(needed, entry->required->required_columns);
        }
    }

    /*
     * Safety check for multi-table joins with ORDER BY + LIMIT:
     * Disable column pruning entirely to avoid missing intermediate join keys.
     * This is a conservative approach for the edge case of 3+ table joins.
     */
    if (rel->has_eclass_joins &&
        ctx->root->parse->sortClause != NIL &&
        ctx->root->parse->limitCount != NULL)
    {
        int num_rels = bms_num_members(ctx->root->all_baserels);

        if (num_rels >= 3)
        {
            CASCADES_DEBUG(ctx->debug, "Cascades: PruneScanColumns disabled for "
                     "multi-table join (n=%d) with ORDER+LIMIT to preserve join keys",
                     num_rels);

            bms_free(needed);
            return NIL;
        }
    }

    if (needed == NULL)
    {
        bms_free(needed);
        return NIL;
    }

    /* Build pruned reltargetlist: keep only vars in needed */
    {
        bool any_attr_needed = false;

        foreach(lc, rel->reltargetlist)
        {
            Var *var = (Var *) lfirst(lc);
            int ndx = var->varattno - rel->min_attr;

            /*
             * Phase 6: Always keep columns that the standard planner marked
             * as needed (attr_needed bitmap non-empty).  These columns are
             * required by join quals at higher levels.  Track whether any
             * column has attr_needed set — if none do, the requirements
             * are incomplete and pruning is unsafe.
             */
            if (ndx >= 0 && ndx < rel->max_attr - rel->min_attr + 1)
            {
                if (!bms_is_empty(rel->attr_needed[ndx]))
                {
                    any_attr_needed = true;
                    CASCADES_DEBUG(ctx->debug, "Cascades: PruneScanColumns keeping varno=%d varattno=%d — attr_needed",
                             (int)var->varno, (int)var->varattno);
                    new_tlist = lappend(new_tlist, var);
                    continue;  /* keep: needed by some join */
                }
            }

            if (bms_is_member(var->varattno, needed))
                new_tlist = lappend(new_tlist, var);
            else
                pruned = true;
        }

        /*
         * Safety: if NO column had attr_needed set, PG hasn't confirmed
         * which columns are join-needed.  Pruning in this state can remove
         * columns that upper plan nodes still reference, causing
         * "variable not found in subplan target lists" errors.
         * Skip pruning entirely — even if output_columns exist, they
         * may be incomplete (missing join keys from other tables).
         */
        if (!any_attr_needed)
        {
            list_free(new_tlist);
            bms_free(needed);
            return NIL;
        }

        /*
         * Additional safety: disable column pruning for scans that feed
         * into multi-table joins (3+ tables) with ORDER BY + LIMIT.
         * These scenarios are prone to missing intermediate join keys.
         *
         * Detection heuristic: if this relation participates in joins
         * and the query has both ORDER BY and LIMIT, be conservative.
         */
        if (rel->has_eclass_joins &&
            ctx->root->parse->sortClause != NIL &&
            ctx->root->parse->limitCount != NULL)
        {
            /* Count number of joined relations */
            int num_rels = bms_num_members(ctx->root->all_baserels);

            if (num_rels >= 3 && pruned)
            {
                CASCADES_DEBUG(ctx->debug, "Cascades: PruneScanColumns skipping pruning for "
                         "multi-table join (n=%d) with ORDER+LIMIT", num_rels);

                list_free(new_tlist);
                bms_free(needed);
                return NIL;
            }
        }
    }

    /*
     * Phase 6: Safety check — never reduce to 0 columns.
     * A scan producing no columns breaks PG's create_plan and executor.
     */
    if (pruned && list_length(new_tlist) > 0)
    {
        rel->reltargetlist = new_tlist;
        /* Re-estimate: fewer columns → narrower rows */
        set_baserel_size_estimates(ctx->root, rel);

        CASCADES_DEBUG(ctx->debug, "Cascades: PruneScanColumns reduced reltargetlist "
                 "for rel %d to %d columns",
                 rel->relid, list_length(new_tlist));
    }

    bms_free(needed);
    return NIL;  /* side-effect only */
}

/*
 * B2: PruneJoinColumns
 *   LogicalJoin(A, B): keep only join key columns + parent-required columns.
 *   Sets group->logical_prop.output_columns to the restricted column set.
 *   B1 (PruneScanColumns) reads this to prune reltargetlist lower down.
 */
static List *
pg_rule_prune_join_columns(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgJoinPrivate *join_priv = (PgJoinPrivate *) expr->op_private;
    Bitmapset  *needed = NULL;
    ListCell   *lc;

    if (join_priv == NULL)
        return NIL;

    /* Collect columns from join quals (join key columns) */
    foreach(lc, join_priv->restrictlist)
    {
        RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
        pg_collect_all_var_attnos((Node *) ri->clause, &needed);
    }

    /*
     * Also propagate parent-required output_columns (from B4/B3/B5)
     * down to children so B1 can see them.  Join quals (restrictlist)
     * may be empty since PG handles quals internally via RelOptInfo.
     */
    if (expr->owner_group->logical_prop.output_columns != NULL)
    {
        needed = bms_union(needed,
                    expr->owner_group->logical_prop.output_columns);
    }

    if (needed != NULL)
    {
        expr->owner_group->logical_prop.output_columns = needed;

        /* Propagate needed columns down to each child so B1 can see them */
        foreach(lc, expr->inputs)
        {
            PgMemoGroup *child = (PgMemoGroup *) lfirst(lc);
            if (child->logical_prop.output_columns != NULL)
                child->logical_prop.output_columns =
                    bms_union(child->logical_prop.output_columns, needed);
            else
                child->logical_prop.output_columns = bms_copy(needed);
        }
    }
    return NIL;
}

/*
 * A4: PushDownPredicateProject
 *   LogicalFilter(LogicalProject(A)) → LogicalProject(LogicalFilter(A))
 *   将 Filter 穿过 Project 下推。仅当 filter 列在 project 输出中时安全。
 */
static List *
pg_rule_pushdown_predicate_project(PgPlannerCascadesContext *ctx,
                                    PgGroupExpr *expr)
{
    PgGroupExpr *proj_expr;
    List *filter_quals;
    PgGroupExpr *new_filter, *new_proj;

    proj_expr = pg_memo_group_first_logical(
        (PgMemoGroup *) linitial(expr->inputs), PG_CASCADES_LOGICAL_PROJECT);
    if (proj_expr == NULL)
        return NIL;

    filter_quals = (List *) expr->op_private;
    if (filter_quals == NIL)
        return NIL;

    /* Check volatile — don't move volatile functions */
    {
        ListCell *lc;
        foreach(lc, filter_quals)
        {
            RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
            if (contain_volatile_functions((Node *) ri->clause))
                return NIL;
        }
    }

    /* Build: Project(Filter(A)) where Filter wraps Project's child */
    new_filter = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_FILTER);
    new_filter->inputs = proj_expr->inputs;  /* Project's child */
    new_filter->op_private = filter_quals;

    new_proj = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_PROJECT);
    new_proj->op_private = proj_expr->op_private;  /* tlist */

    return list_make1(new_proj);  /* new_proj's input = new_filter's group */
}

/*
 * D2: PushDownLimitJoin
 *   LogicalLimit(LogicalJoin(A,B)) → LogicalJoin(LogicalLimit(A), LogicalLimit(B))
 *   仅对 INNER JOIN 生效。将 LIMIT 下推到两侧。
 */
static List *
pg_rule_pushdown_limit_join(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *join_expr;
    PgJoinPrivate *join_priv;
    PgGroupExpr *new_limit_outer, *new_limit_inner;
    PgGroupExpr *new_join;

    join_expr = pg_memo_group_first_logical(
        (PgMemoGroup *) linitial(expr->inputs), PG_CASCADES_LOGICAL_JOIN);
    if (join_expr == NULL)
        return NIL;

    join_priv = (PgJoinPrivate *) join_expr->op_private;
    if (join_priv == NULL || join_priv->jointype != JOIN_INNER)
        return NIL;

    /* Only push if we have a meaningful limit */
    if (ctx->upper->limit_tuples <= 0)
        return NIL;

    /* Create LogicalLimit wrappers for both children */
    new_limit_outer = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_LIMIT);
    new_limit_outer->inputs = list_make1(linitial(join_expr->inputs));
    new_limit_outer->op_private = ctx->upper;

    new_limit_inner = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_LIMIT);
    new_limit_inner->inputs = list_make1(lsecond(join_expr->inputs));
    new_limit_inner->op_private = ctx->upper;

    /* Rebuild Join with original children (Limit pushdown is semantic:
     * the Limit wrappers are inserted into children's groups,
     * and cost-based search will use them when beneficial.) */
    new_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    new_join->inputs = list_make2(linitial(join_expr->inputs),
                                   lsecond(join_expr->inputs));
    new_join->op_private = join_priv;

    /*
     * Return all three: Limit wrappers get inserted into the group first,
     * then the new_join references the same child groups.  The engine
     * processes them sequentially so Limit wrappers exist before new_join
     * is optimized.
     */
    return list_make3(new_limit_outer, new_limit_inner, new_join);
}

/*
 * A3: PushDownPredicateAgg
 *   LogicalFilter(LogicalAgg(A)) → LogicalAgg(LogicalFilter(A))
 *   将 filter 转为 HAVING（仅当 filter 列在 GROUP BY 中时安全）。
 */
static List *
pg_rule_pushdown_predicate_agg(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *agg_expr;
    List       *filter_quals;

    agg_expr = pg_memo_group_first_logical(
        (PgMemoGroup *) linitial(expr->inputs), PG_CASCADES_LOGICAL_AGG);
    if (agg_expr == NULL)
        return NIL;

    filter_quals = (List *) expr->op_private;
    if (filter_quals == NIL)
        return NIL;

    /* Check volatile */
    {
        ListCell *lc;
        foreach(lc, filter_quals)
        {
            RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
            if (contain_volatile_functions((Node *) ri->clause))
                return NIL;
        }
    }

    /* Build: Agg(Filter(A)) — move filter below Agg */
    {
        PgGroupExpr *new_filter = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_FILTER);
        new_filter->inputs = agg_expr->inputs;
        new_filter->op_private = filter_quals;

        PgGroupExpr *new_agg = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_AGG);
        new_agg->op_private = agg_expr->op_private;
        return list_make1(new_agg);
    }
}

/*
 * F3: PushDownAggLimit
 *   LogicalAgg(LogicalLimit(A)) → LogicalLimit(LogicalAgg(A))
 *   交换 Agg 和 Limit：先聚合再限制，通常更高效。
 */
static List *
pg_rule_pushdown_agg_limit(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *limit_expr;

    limit_expr = pg_memo_group_first_logical(
        (PgMemoGroup *) linitial(expr->inputs), PG_CASCADES_LOGICAL_LIMIT);
    if (limit_expr == NULL)
        return NIL;

    /* Build: Limit(Agg(A)) */
    {
        PgGroupExpr *new_agg = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_AGG);
        new_agg->inputs = limit_expr->inputs;
        new_agg->op_private = expr->op_private;

        PgGroupExpr *new_limit = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_LIMIT);
        new_limit->op_private = limit_expr->op_private;
        return list_make1(new_limit);
    }
}

/*
 * G1: EliminateJoinWithConstant
 *   LogicalJoin(A, const_B) where B has rows==1 → eliminate join.
 *   First version: only when one side is provably single-row.
 */
static List *
pg_rule_eliminate_join_with_constant(PgPlannerCascadesContext *ctx,
                                      PgGroupExpr *expr)
{
    PgJoinPrivate *join_priv;
    PgMemoGroup *outer_grp, *inner_grp;

    join_priv = (PgJoinPrivate *) expr->op_private;
    if (join_priv == NULL || join_priv->jointype != JOIN_INNER)
        return NIL;

    outer_grp = (PgMemoGroup *) linitial(expr->inputs);
    inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

    /* Check if either side is a single-row relation */
    if (outer_grp->rel != NULL && outer_grp->rel->rows <= 1.0 &&
        outer_grp->rel->tuples <= 1)
    {
        /* Outer is constant — return inner directly via Project */
        PgGroupExpr *new_proj = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_PROJECT);
        new_proj->inputs = list_make1(inner_grp);
        new_proj->op_private = ctx->upper->tlist;
        return list_make1(new_proj);
    }

    if (inner_grp->rel != NULL && inner_grp->rel->rows <= 1.0 &&
        inner_grp->rel->tuples <= 1)
    {
        PgGroupExpr *new_proj = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_PROJECT);
        new_proj->inputs = list_make1(outer_grp);
        new_proj->op_private = ctx->upper->tlist;
        return list_make1(new_proj);
    }

    return NIL;
}

/*
 * G2: OuterJoinElimination
 *   LogicalLeftJoin(A, B) where B columns are unreferenced → InnerJoin or drop B.
 *   First version: always convert LEFT to INNER (conservative).
 */
static List *
pg_rule_outer_join_elimination(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgJoinPrivate *join_priv;
    PgJoinPrivate *new_priv;
    PgGroupExpr *new_join;

    join_priv = (PgJoinPrivate *) expr->op_private;
    if (join_priv == NULL)
        return NIL;

    /* Only transform LEFT JOIN */
    if (join_priv->jointype != JOIN_LEFT)
        return NIL;

    /* First version: always convert to INNER (PG's reduce_outer_joins
     * already handles the truly-eliminable cases) */
    new_priv = (PgJoinPrivate *) palloc(sizeof(PgJoinPrivate));
    memcpy(new_priv, join_priv, sizeof(PgJoinPrivate));
    new_priv->jointype = JOIN_INNER;

    new_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    new_join->inputs = expr->inputs;
    new_join->op_private = new_priv;

    return list_make1(new_join);
}

/*
 * A2: PushDownPredicateJoin (INNER JOIN only)
 *   LogicalFilter(LogicalJoin(A, B)) → LogicalJoin(Filter(A), Filter(B))
 *   将 WHERE 条件下推到 Join 两侧。第一版仅对 INNER JOIN 生效。
 */
static List *
pg_rule_pushdown_predicate_join(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *join_expr;
    PgJoinPrivate *join_priv;
    List       *filter_quals;
    PgMemoGroup *outer_grp, *inner_grp;
    Relids      outer_relids, inner_relids;
    List       *outer_quals = NIL;
    List       *inner_quals = NIL;
    ListCell   *lc;
    PgGroupExpr *new_outer, *new_inner, *new_join;

    join_expr = pg_memo_group_first_logical(
        (PgMemoGroup *) linitial(expr->inputs), PG_CASCADES_LOGICAL_JOIN);
    if (join_expr == NULL)
        return NIL;

    join_priv = (PgJoinPrivate *) join_expr->op_private;
    if (join_priv == NULL || join_priv->jointype != JOIN_INNER)
        return NIL;  /* first version: INNER JOIN only */

    filter_quals = (List *) expr->op_private;
    if (filter_quals == NIL)
        return NIL;

    if (list_length(join_expr->inputs) != 2)
        return NIL;
    outer_grp = (PgMemoGroup *) linitial(join_expr->inputs);
    inner_grp = (PgMemoGroup *) lsecond(join_expr->inputs);

    outer_relids = pg_cascades_group_relids(ctx, outer_grp);
    inner_relids = pg_cascades_group_relids(ctx, inner_grp);

    foreach(lc, filter_quals)
    {
        RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

        if (contain_volatile_functions((Node *) ri->clause))
            continue;

        /* Push to outer if qual only references outer relids */
        if (bms_is_subset(ri->clause_relids, outer_relids))
            outer_quals = lappend(outer_quals, ri);

        /* Push to inner if qual only references inner relids */
        if (bms_is_subset(ri->clause_relids, inner_relids))
            inner_quals = lappend(inner_quals, ri);
    }

    if (outer_quals == NIL && inner_quals == NIL)
        return NIL;

    /* Build Filter wrappers for sides with pushed quals */
    new_outer = NULL;
    new_inner = NULL;

    if (outer_quals != NIL)
    {
        new_outer = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_FILTER);
        new_outer->inputs = list_make1(outer_grp);
        new_outer->op_private = outer_quals;
    }

    if (inner_quals != NIL)
    {
        new_inner = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_FILTER);
        new_inner->inputs = list_make1(inner_grp);
        new_inner->op_private = inner_quals;
    }

    /* Rebuild Join */
    new_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    new_join->op_private = join_priv;

    return list_make1(new_join);
}

/*
 * C2: JoinAssociativity
 *   (A⋈B)⋈C → A⋈(B⋈C)
 *
 *   StarRocks reference: Memo.copyIn() handles recursive expression-to-group
 *   conversion.  In PG, we call pg_memo_insert_expression() inside the
 *   transform to create the intermediate (B⋈C) group, then build the
 *   outer A⋈(B⋈C) referencing that group.
 *
 *   Prerequisite: left input group must contain at least one JOIN expression
 *   whose children are base groups (A, B).  Right input is group C.
 *
 *   Qual handling (first version): quals are looked up from the PG joinrel
 *   at physical implementation time via group->rel.  The structural
 *   transform is qual-agnostic.
 */
static List *
pg_rule_join_associativity(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *left_grp, *right_grp;
    PgGroupExpr *inner_join;
    PgMemoGroup *A_grp, *B_grp, *C_grp;
    PgGroupExpr *bc_join, *abc_join;
    PgMemoGroup *bc_group;
    ListCell   *lc;

    /* expr is (A⋈B)⋈C: JOIN with 2 inputs */
    left_grp = (PgMemoGroup *) linitial(expr->inputs);
    right_grp = (PgMemoGroup *) lsecond(expr->inputs);

    if (left_grp == NULL || right_grp == NULL)
        return NIL;

    /* Find a JOIN expression inside left_grp.
     * After rewrite pipeline + Final Cleanup group merge, LOGICAL_JOIN
     * may have been eliminated from logical_exprs.  Search physical_exprs
     * too — COMPOSABLE_OP join expressions have valid child inputs. */
    inner_join = NULL;
    foreach(lc, left_grp->logical_exprs)
    {
        PgGroupExpr *e = (PgGroupExpr *) lfirst(lc);
        if (e->op == PG_CASCADES_LOGICAL_JOIN &&
            list_length(e->inputs) == 2)
        {
            inner_join = e;
            break;
        }
    }

    if (inner_join == NULL)
    {
        /* Fallback: if group merge eliminated LOGICAL_JOIN from logical_exprs,
         * construct inner join from left_grp's PG joinrel.  Iterate base
         * groups in memo to find A and B by matching relids. */
        Relids left_relids = pg_cascades_group_relids(ctx, left_grp);
        if (left_relids != NULL)
        {
            PgMemoGroup *found[2];
            int nfound = 0;
            ListCell *gc;

            MemSet(found, 0, sizeof(found));
            foreach(gc, ctx->memo->groups)
            {
                PgMemoGroup *g = (PgMemoGroup *) lfirst(gc);
                if (g != left_grp && g != right_grp &&
                    g->rel != NULL && g->rel->relids != NULL &&
                    bms_overlap(g->rel->relids, left_relids))
                {
                    if (nfound < 2)
                        found[nfound++] = g;
                }
            }

            if (nfound >= 2 && found[0] != NULL && found[1] != NULL)
            {
                inner_join = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_JOIN);
                inner_join->inputs = list_make2(found[0], found[1]);
            }
        }
        if (left_relids != NULL)
            bms_free(left_relids);
    }

    if (inner_join == NULL)
        return NIL;

    A_grp = (PgMemoGroup *) linitial(inner_join->inputs);
    B_grp = (PgMemoGroup *) lsecond(inner_join->inputs);
    C_grp = right_grp;

    if (A_grp == NULL || B_grp == NULL || C_grp == NULL)
        return NIL;

    /* Avoid degenerate: don't reassociate if any two inputs are the same */
    if (A_grp == B_grp || A_grp == C_grp || B_grp == C_grp)
        return NIL;

    /*
     * Step 1: Create (B⋈C), insert into Memo to get its group.
     * StarRocks copyIn pattern: transform returns a tree of OptExpression,
     * copyIn recursively creates groups.  Here we do it explicitly.
     */
    bc_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    bc_join->inputs = list_make2(B_grp, C_grp);
    /* op_private stays NULL — quals resolved from PG joinrel at impl time */

    bc_group = pg_memo_insert_expression(ctx, ctx->memo, bc_join, NULL);
    if (bc_group == NULL)
        return NIL;  /* (B⋈C) already exists or duplicate */

    /*
     * Step 2: Create A⋈(B⋈C) with bc_group as input.
     * The engine will push ExploreGroupTask for bc_group when this
     * expression is optimized.
     */
    abc_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    abc_join->inputs = list_make2(A_grp, bc_group);

    return list_make1(abc_join);
}

/*
 * C3: JoinLeftAsscom
 *   A⋈(B⋈C) → (A⋈B)⋈C
 *
 *   Mirror of C2.  When the right input group contains a JOIN expression,
 *   pull it out to form a left-deep tree.
 */
static List *
pg_rule_join_left_asscom(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *left_grp, *right_grp;
    PgGroupExpr *inner_join;
    PgMemoGroup *A_grp, *B_grp, *C_grp;
    PgGroupExpr *ab_join, *abc_join;
    PgMemoGroup *ab_group;
    ListCell   *lc;

    /* expr is A⋈(B⋈C): JOIN with 2 inputs */
    left_grp = (PgMemoGroup *) linitial(expr->inputs);
    right_grp = (PgMemoGroup *) lsecond(expr->inputs);

    if (left_grp == NULL || right_grp == NULL)
        return NIL;

    /* Find a JOIN expression inside right_grp */
    inner_join = NULL;
    foreach(lc, right_grp->logical_exprs)
    {
        PgGroupExpr *e = (PgGroupExpr *) lfirst(lc);
        if (e->op == PG_CASCADES_LOGICAL_JOIN &&
            list_length(e->inputs) == 2)
        {
            inner_join = e;
            break;
        }
    }
    /* Relids-based fallback: same as JoinAssociativity (C2).
     * After Final Cleanup group merge, LOGICAL_JOIN may be gone from
     * logical_exprs.  Find base groups by matching relids. */
    if (inner_join == NULL)
    {
        Relids right_relids = pg_cascades_group_relids(ctx, right_grp);
        if (right_relids != NULL)
        {
            PgMemoGroup *found[2];
            int nfound = 0;
            ListCell *gc;

            MemSet(found, 0, sizeof(found));
            foreach(gc, ctx->memo->groups)
            {
                PgMemoGroup *g = (PgMemoGroup *) lfirst(gc);
                if (g != left_grp && g != right_grp &&
                    g->rel != NULL && g->rel->relids != NULL &&
                    bms_overlap(g->rel->relids, right_relids))
                {
                    if (nfound < 2)
                        found[nfound++] = g;
                }
            }

            if (nfound >= 2 && found[0] != NULL && found[1] != NULL)
            {
                inner_join = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_JOIN);
                inner_join->inputs = list_make2(found[0], found[1]);
            }
        }
        if (right_relids != NULL)
            bms_free(right_relids);
    }

    if (inner_join == NULL)
        return NIL;

    A_grp = left_grp;
    B_grp = (PgMemoGroup *) linitial(inner_join->inputs);
    C_grp = (PgMemoGroup *) lsecond(inner_join->inputs);

    if (A_grp == NULL || B_grp == NULL || C_grp == NULL)
        return NIL;

    /* Avoid degenerate */
    if (A_grp == B_grp || A_grp == C_grp || B_grp == C_grp)
        return NIL;

    /*
     * Step 1: Create (A⋈B), insert to get its group.
     */
    ab_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    ab_join->inputs = list_make2(A_grp, B_grp);

    ab_group = pg_memo_insert_expression(ctx, ctx->memo, ab_join, NULL);
    if (ab_group == NULL)
        return NIL;

    /*
     * Step 2: Create (A⋈B)⋈C with ab_group as left input.
     */
    abc_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    abc_join->inputs = list_make2(ab_group, C_grp);

    return list_make1(abc_join);
}

/*
 * G3: InnerJoinToSemi
 *   LogicalJoin(INNER, A, B) → LogicalJoin(SEMI, A, B)
 *
 *   Applies when the inner (right) side of an INNER JOIN has a unique key
 *   on the join columns, meaning the join cannot produce duplicate rows.
 *   In this case, a SEMI JOIN is semantically equivalent and can be
 *   cheaper (early stop).
 *
 *   First version: checks group->rel for unique indexes.  If the inner
 *   group maps to a base relation with a primary key or unique index,
 *   we conservatively convert to SEMI.  A stricter version would verify
 *   that the join keys actually cover the unique columns.
 *
 *   Prerequisites:
 *   - expr is a LOGICAL_JOIN with 2 inputs
 *   - Inner group has group->rel != NULL (base relation)
 *   - Inner group->rel has a unique index or primary key
 */
static List *
pg_rule_inner_to_semi(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *inner_grp;
    PgGroupExpr *new_join;
    PgJoinPrivate *join_priv_orig;
    PgJoinPrivate *join_priv_new;
    RelOptInfo  *inner_rel;

    inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

    /*
     * Check join type.  If op_private is NULL (legacy logical joins from
     * pg_adapter), we can't determine join type — skip.
     */
    join_priv_orig = (PgJoinPrivate *) expr->op_private;
    if (join_priv_orig == NULL)
        return NIL;

    if (join_priv_orig->jointype != JOIN_INNER)
        return NIL;

    /*
     * Check if inner side has a unique key.
     * For base relations: check group->rel for unique indexes.
     * For join relations: skip (first version).
     */
    inner_rel = inner_grp->rel;
    if (inner_rel == NULL)
        return NIL;

    /*
     * PostgreSQL stores unique indexes in the relation's indexlist.
     * We check if any index on this relation is unique/primary.
     * This is conservative: we don't verify that the join keys
     * actually match the unique columns.  A stricter check would
     * require comparing the join quals against the index columns.
     */
    if (inner_rel->indexlist != NIL)
    {
        ListCell *lc;
        bool has_unique = false;

        foreach(lc, inner_rel->indexlist)
        {
            IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
            if (idx->unique || idx->ncolumns == 1)
            {
                has_unique = true;
                break;
            }
        }

        if (!has_unique)
            return NIL;
    }
    else
    {
        /* No indexes at all — can't prove uniqueness */
        return NIL;
    }

    /*
     * Convert to SEMI: copy the join expr with jointype changed.
     * op_private is shallow-copied; we create a new PgJoinPrivate
     * to avoid mutating the original.
     */
    new_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    new_join->inputs = list_copy(expr->inputs);
    join_priv_new = (PgJoinPrivate *) palloc(sizeof(PgJoinPrivate));
    memcpy(join_priv_new, join_priv_orig, sizeof(PgJoinPrivate));
    join_priv_new->jointype = JOIN_SEMI;
    new_join->op_private = join_priv_new;

    return list_make1(new_join);
}

/*
 * D3: EliminateLimit
 *   LogicalLimit(A) where no LIMIT/OFFSET → merge child group into parent.
 *
 *   When the query has no LIMIT clause (limitCount == NULL && limitOffset == NULL),
 *   the LogicalLimit node is a no-op.  Instead of creating a passthrough
 *   expression, we merge the child group's expressions directly into the
 *   current group — this is the "group merging" pattern.
 *
 *   Returns NIL (side effect: calls pg_memo_merge_group).
 */
static List *
pg_rule_eliminate_limit(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    Query *parse = ctx->root->parse;
    PgMemoGroup *child_group;

    /* Only eliminate if there's actually no LIMIT/OFFSET */
    if (parse->limitCount != NULL || parse->limitOffset != NULL)
        return NIL;

    child_group = (PgMemoGroup *) linitial(expr->inputs);
    if (child_group == NULL || child_group == expr->owner_group)
        return NIL;

    /* Merge child group's expressions into the parent group */
    pg_memo_merge_group(ctx, expr->owner_group, child_group);

    /*
     * Phase 6: After merging, remove the eliminated expression (LogicalLimit)
     * from the target group.  Its inputs still point to the now-empty source
     * group, and keeping it would cause planbuild failures when the task
     * scheduler tries to optimize a child with no expressions.
     */
    expr->owner_group->logical_exprs = list_delete_ptr(
        expr->owner_group->logical_exprs, expr);

    /* Return NIL — no new expressions to insert; group merging was a side effect */
    return NIL;
}

/*
 * H5: MergeLimitWithChildLimit
 *   LogicalLimit(LogicalLimit(A)) → LogicalLimit(A)
 *
 *   When two consecutive Limit nodes appear (e.g., from a rule that
 *   wraps a Limit around an expression that already has a Limit),
 *   merge them by taking the stricter limit.
 *
 *   Pattern: LogicalLimit(child is a group containing LogicalLimit)
 */
static List *
pg_rule_merge_limit_with_child_limit(PgPlannerCascadesContext *ctx,
                                      PgGroupExpr *expr)
{
    PgMemoGroup *child_group;
    PgGroupExpr *inner_limit = NULL;
    ListCell *lc;

    child_group = (PgMemoGroup *) linitial(expr->inputs);

    /* Find a LogicalLimit in the child group */
    foreach(lc, child_group->logical_exprs)
    {
        PgGroupExpr *e = (PgGroupExpr *) lfirst(lc);
        if (e->op == PG_CASCADES_LOGICAL_LIMIT)
        {
            inner_limit = e;
            break;
        }
    }

    if (inner_limit == NULL)
        return NIL;

    /*
     * Merge: create a new Limit that takes the stricter of the two.
     * Since both Limits come from the same query's LIMIT clause
     * (or from rule applications), they should have the same limit
     * values.  We just pass through to the inner Limit's child.
     */
    {
        PgGroupExpr *new_limit = pg_memo_new_group_expr(ctx,
                                        PG_CASCADES_LOGICAL_LIMIT);
        new_limit->inputs = inner_limit->inputs;  /* skip inner Limit */
        new_limit->op_private = expr->op_private; /* keep outer info */
        return list_make1(new_limit);
    }
}

/*
 * F1: EliminateAgg
 *   LogicalAgg(A) where no aggregation + no GROUP BY → merge child into parent.
 *
 *   When the query has no aggregate functions (hasAggs == false) and no
 *   GROUP BY clause, the LogicalAgg node is a no-op.  Merge the child
 *   group's expressions directly into the current group.
 *
 *   Returns NIL (side effect: calls pg_memo_merge_group).
 */
static List *
pg_rule_eliminate_agg(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *child_group;

    /* Only eliminate if there's no aggregation and no GROUP BY */
    if (ctx->root->parse->hasAggs || ctx->root->parse->groupClause != NIL)
        return NIL;

    child_group = (PgMemoGroup *) linitial(expr->inputs);
    if (child_group == NULL || child_group == expr->owner_group)
        return NIL;

    /* Merge child group's expressions into the parent group */
    pg_memo_merge_group(ctx, expr->owner_group, child_group);

    /*
     * Phase 6: Remove the eliminated expression from the target group.
     * See pg_rule_eliminate_limit for rationale.
     */
    expr->owner_group->logical_exprs = list_delete_ptr(
        expr->owner_group->logical_exprs, expr);

    /* Return NIL — no new expressions to insert */
    return NIL;
}

/*
 * EnforceSort (PG_RULE_ENFORCER):
 *   Insert PhysicalSort on top of a child group when required pathkeys
 *   are not satisfied by the child's output.
 *
 *   This is the canonical enforcer rule.  It is triggered by the
 *   ENFORCE_ENFORCE_PROPERTY state in EnforceAndCostTask when
 *   pg_output_satisfies_required() returns false.
 *
 *   Pattern: leaf (matches any group)
 *   Transform: creates PhysicalSort(child) with pathkeys from required property
 */
static List *
pg_rule_enforce_sort(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    /*
     * This transform is a no-op at the rule level.  The actual Sort
     * enforcer is created by the EnforceAndCostTask state machine in
     * task.c (ENFORCE_ENFORCE_PROPERTY state), which has access to the
     * required property's pathkeys that the rule transform cannot see.
     *
     * The rule's purpose is to be registered as PG_RULE_ENFORCER so the
     * framework knows Sort enforcement is available.
     */
    (void) expr;
    return NIL;
}


/* ========================================================================
 * Phase 5: Transformation rules (29 rules)
 *   C2/C3/G3/D3/F1/A5 added.
 * ======================================================================== */

static PgRule g_trans_rules_phase5[] = {
    /* H2: MergeProjectWithChild — LogicalProject(LogicalProject(A)) → LogicalProject(A) */
    {"MergeProjectWithChild", NULL, pg_rule_merge_project_with_child,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_PROJECT, 0, PG_RULE_BIT_MERGE_PROJECT, 0.5},

    /* E2: PruneEmptyScan — mark empty scan (side-effect only) */
    {"PruneEmptyScan", NULL, pg_rule_prune_empty_scan,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_SCAN, 0, PG_RULE_BIT_PRUNE_EMPTY_SCAN, 0.9},

    /* H3: EliminateProject — remove no-op Project */
    {"EliminateProject", NULL, pg_rule_eliminate_project,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_PROJECT, 0, PG_RULE_BIT_ELIMINATE_PROJECT, 0.6},

    /* A1: PushDownPredicateScan — filter into scan qual */
    {"PushDownPredicateScan", NULL, pg_rule_pushdown_predicate_scan,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_FILTER, 0, PG_RULE_BIT_PUSHDOWN_PRED_SCAN, 0.6},

    /* D1: MergeLimitWithSort — eliminate redundant Limit on top of Sort */
    {"MergeLimitWithSort", NULL, pg_rule_merge_limit_with_sort,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_LIMIT, 0, PG_RULE_BIT_MERGE_LIMIT_SORT, 0.7},

    /* H1: EliminateSortWithConstantKey — remove Sort on constant keys */
    {"EliminateSortWithConstKey", NULL, pg_rule_eliminate_sort_with_constant_key,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_SORT, 0, PG_RULE_BIT_ELIM_SORT_CONST_KEY, 0.5},

    /* H4: MergeFilterWithJoin — merge Filter into Join qual (INNER only) */
    {"MergeFilterWithJoin", NULL, pg_rule_merge_filter_with_join,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_JOIN, 0, PG_RULE_BIT_MERGE_FILTER_JOIN, 0.4},

    /* E3: PruneEmptyUnion — remove empty branches from Union */
    {"PruneEmptyUnion", NULL, pg_rule_prune_empty_union,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_PROJECT, 0, PG_RULE_BIT_PRUNE_EMPTY_UNION, 0.8},

    /* F2: MergeTwoAgg — merge two consecutive Agg operators */
    {"MergeTwoAgg", NULL, pg_rule_merge_two_agg,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_AGG, 0, PG_RULE_BIT_MERGE_TWO_AGG, 0.5},

    /* G4: MergeJoinWithChildProject — eliminate Project under Join */
    {"MergeJoinWithChildProj", NULL, pg_rule_merge_join_with_child_project,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_JOIN, 0, PG_RULE_BIT_MERGE_JOIN_PROJ, 0.4},

    /* B3: PruneAggColumns — reduce Agg to GROUP BY + aggre cols */
    {"PruneAggColumns", NULL, pg_rule_prune_agg_columns,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_AGG, 0, PG_RULE_BIT_PRUNE_AGG_COLS, 0.5},

    /* B4: PruneProjectColumns — keep only parent-referenced columns */
    {"PruneProjectColumns", NULL, pg_rule_prune_project_columns,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_PROJECT, 0, PG_RULE_BIT_PRUNE_PROJ_COLS, 0.6},

    /* B5: PruneSortColumns — keep only sort key + parent columns */
    {"PruneSortColumns", NULL, pg_rule_prune_sort_columns,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_SORT, 0, PG_RULE_BIT_PRUNE_SORT_COLS, 0.5},

    /* E1: PruneEmptyJoin — mark join as empty if either child is empty */
    {"PruneEmptyJoin", NULL, pg_rule_prune_empty_join,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_JOIN, 0, PG_RULE_BIT_PRUNE_EMPTY_JOIN, 0.9},

    /* B1: PruneScanColumns — reduce scan to needed columns
     * RE-ENABLED: C3203 issue has been resolved with proper fallback checks
     */
    {"PruneScanColumns", NULL, pg_rule_prune_scan_columns,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_SCAN, 0, PG_RULE_BIT_PRUNE_SCAN_COLS, 0.6},

    /* B2: PruneJoinColumns — reduce join to key + parent columns
     * RE-ENABLED: C3203 issue has been resolved with proper fallback checks
     */
    {"PruneJoinColumns", NULL, pg_rule_prune_join_columns,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_JOIN, 0, PG_RULE_BIT_PRUNE_JOIN_COLS, 0.5},

    /* A4: PushDownPredicateProject — filter through Project */
    {"PushDownPredicateProject", NULL, pg_rule_pushdown_predicate_project,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_FILTER, 0, PG_RULE_BIT_PUSHDOWN_PRED_PROJ, 0.5},

    /* D2: PushDownLimitJoin — push Limit into Join children (INNER only) */
    {"PushDownLimitJoin", NULL, pg_rule_pushdown_limit_join,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_LIMIT, 0, PG_RULE_BIT_PUSHDOWN_LIMIT_JOIN, 0.4},

    /* A3: PushDownPredicateAgg — filter through Agg (turn into HAVING-like) */
    {"PushDownPredicateAgg", NULL, pg_rule_pushdown_predicate_agg,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_FILTER, 0, PG_RULE_BIT_PUSHDOWN_PRED_AGG, 0.3},

    /* F3: PushDownAggLimit — swap Agg and Limit */
    {"PushDownAggLimit", NULL, pg_rule_pushdown_agg_limit,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_AGG, 0, PG_RULE_BIT_PUSHDOWN_AGG_LIMIT, 0.4},

    /* G1: EliminateJoinWithConstant — drop join with single-row side */
    {"EliminateJoinWithConst", NULL, pg_rule_eliminate_join_with_constant,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_JOIN, 0, PG_RULE_BIT_ELIM_JOIN_CONST, 0.7},

    /* G2: OuterJoinElimination — LEFT→INNER conversion */
    {"OuterJoinElimination", NULL, pg_rule_outer_join_elimination,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_JOIN, 0, PG_RULE_BIT_OUTER_JOIN_ELIM, 0.6},

    /* A2: PushDownPredicateJoin — push WHERE into Join children (INNER only) */
    {"PushDownPredicateJoin", NULL, pg_rule_pushdown_predicate_join,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_FILTER, 0, PG_RULE_BIT_PUSHDOWN_PRED_JOIN, 0.4},

    /* G3: InnerToSemi — convert INNER JOIN to SEMI JOIN when inner side unique */
    {"InnerToSemi", NULL, pg_rule_inner_to_semi,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_JOIN, 0, PG_RULE_BIT_INNER_TO_SEMI, 0.3},

    /* C2: JoinAssociativity — (A⋈B)⋈C → A⋈(B⋈C) */
    {"JoinAssociativity", NULL, pg_rule_join_associativity,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_JOIN, 0, PG_RULE_BIT_JOIN_ASSOCIATIVITY, 0.2},

    /* C3: JoinLeftAsscom — A⋈(B⋈C) → (A⋈B)⋈C */
    {"JoinLeftAsscom", NULL, pg_rule_join_left_asscom,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_JOIN, 0, PG_RULE_BIT_JOIN_LEFT_ASSCOM, 0.2},

    /* D3: EliminateLimit — remove no-op Limit when no LIMIT clause */
    {"EliminateLimit", NULL, pg_rule_eliminate_limit,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_LIMIT, 0, PG_RULE_BIT_ELIMINATE_LIMIT, 0.6},

    /* H5: MergeLimitWithChildLimit — merge consecutive Limit nodes */
    {"MergeLimitWithChildLimit", NULL, pg_rule_merge_limit_with_child_limit,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_LIMIT, 0, PG_RULE_BIT_MERGE_LIMIT_CHILD_LIMIT, 0.45},

    /* F1: EliminateAgg — remove no-op Agg when no aggregation */
    {"EliminateAgg", NULL, pg_rule_eliminate_agg,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_AGG, 0, PG_RULE_BIT_ELIMINATE_AGG, 0.6},

    {NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0.0}  /* sentinel */
};

/* Phase 5: Enforcer rules */
static PgRule g_enforcer_rules[] = {
    {"EnforceSort", NULL, pg_rule_enforce_sort,
     PG_RULE_ENFORCER, NULL,
     0, PG_CASCADES_PHYSICAL_SORT,
     PG_RULE_BIT_ENFORCE_SORT, 0.0},
    {NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0.0}  /* sentinel */
};

/* ========================================================================
 * Phase 4: Rule sorting by promise
 * ======================================================================== */

static int
pg_rule_promise_compare(const void *a, const void *b)
{
    const PgRule *ra = (const PgRule *) a;
    const PgRule *rb = (const PgRule *) b;

    /* Sort descending: higher promise first */
    if (ra->promise > rb->promise)
        return -1;
    if (ra->promise < rb->promise)
        return 1;
    return 0;
}

PgRule *
pg_cascades_get_rules_sorted(PgRule *rules, int *num_rules)
{
    PgRule *sorted;
    int n = 0;

    /* Count rules (exclude sentinel) */
    while (rules[n].name != NULL)
        n++;

    if (n == 0)
    {
        *num_rules = 0;
        return NULL;
    }

    /* Allocate and copy */
    sorted = (PgRule *) palloc(sizeof(PgRule) * (n + 1));
    memcpy(sorted, rules, sizeof(PgRule) * n);
    /* Sentinel */
    MemSet(&sorted[n], 0, sizeof(PgRule));

    /* Sort by promise descending */
    qsort(sorted, n, sizeof(PgRule), pg_rule_promise_compare);

    *num_rules = n;
    return sorted;
}

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

/* Phase 2: Scan-only rules (safe for Path-import mode, no child groups) */
PgRule *
pg_cascades_get_impl_rules_phase2_scan(int *num_rules)
{
    int i = 0;
    while (g_impl_rules_phase2_scan[i].name != NULL)
        i++;
    *num_rules = i;
    return g_impl_rules_phase2_scan;
}

/* Phase 2: Join-only rules (safe for tree-based Memo with PG Path import) */
PgRule *
pg_cascades_get_impl_rules_phase2_join(int *num_rules)
{
    int i = 0;
    while (g_impl_rules_phase2_join[i].name != NULL)
        i++;
    *num_rules = i;
    return g_impl_rules_phase2_join;
}

/* Phase 4: Path-generation join rules (call make_join_rel internally) */
static PgRule g_impl_rules_phase4_join[] = {
    {"LogicalJoin->PhysicalHashJoin_Phase4", NULL,
     pg_rule_join_to_hashjoin_phase4,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_HASHJOIN,
     PG_RULE_BIT_JOIN_TO_HASHJOIN_PHASE4, 0.9},
    {"LogicalJoin->PhysicalNestLoop_Phase4", NULL,
     pg_rule_join_to_nestloop_phase4,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_NESTLOOP,
     PG_RULE_BIT_JOIN_TO_NESTLOOP_PHASE4, 0.9},
    {"LogicalJoin->PhysicalMergeJoin_Phase4", NULL,
     pg_rule_join_to_mergejoin_phase4,
     PG_RULE_IMPL, NULL, PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_MERGEJOIN,
     PG_RULE_BIT_JOIN_TO_MERGEJOIN_PHASE4, 0.9},
    {NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0.0}  /* sentinel */
};

PgRule *
pg_cascades_get_impl_rules_phase4_join(int *num_rules)
{
    int i = 0;
    while (g_impl_rules_phase4_join[i].name != NULL)
        i++;
    *num_rules = i;
    return g_impl_rules_phase4_join;
}

/* Phase 5: Transformation rules (26 rules) */
PgRule *
pg_cascades_get_trans_rules_phase5(int *num_rules)
{
    int i = 0;
    while (g_trans_rules_phase5[i].name != NULL)
        i++;
    *num_rules = i;
    return g_trans_rules_phase5;
}

/* Phase 5: Enforcer rules */
PgRule *
pg_cascades_get_enforcer_rules(int *num_rules)
{
    int i = 0;
    while (g_enforcer_rules[i].name != NULL)
        i++;
    *num_rules = i;
    return g_enforcer_rules;
}

/* ========================================================================
 * Phase 5: Pattern initialization for multi-node rules
 *
 * Patterns are created once per backend lifetime (static allocation).
 * Single-node rules (pattern=NULL) use from_op matching.
 * Multi-node rules get pattern trees for child-group enumeration.
 * ======================================================================== */

/* Static pattern nodes for multi-node rules */
PgPattern *g_pat_leaf1 = NULL;
PgPattern *g_pat_leaf2 = NULL;
PgPattern *g_pat_join_leaf_leaf = NULL;
PgPattern *g_pat_join_join_leaf_leaf_leaf = NULL;
PgPattern *g_pat_join_leaf_join_leaf_leaf = NULL;
PgPattern *g_pat_filter_join_leaf_leaf = NULL;
PgPattern *g_pat_filter_project_leaf = NULL;
PgPattern *g_pat_limit_sort_leaf = NULL;
PgPattern *g_pat_limit_join_leaf_leaf = NULL;
PgPattern *g_pat_agg_agg_leaf = NULL;
PgPattern *g_pat_agg_limit_leaf = NULL;
PgPattern *g_pat_join_project_leaf_project_leaf = NULL;
PgPattern *g_pat_project_project_leaf = NULL;
PgPattern *g_pat_join_filter_leaf_leaf = NULL;

/*
 * pg_cascades_init_rule_patterns:
 *   Initialize pattern trees for multi-node transformation rules.
 *   Must be called once before any pattern-based rule matching.
 *   Safe to call multiple times (idempotent).
 */
void
pg_cascades_init_rule_patterns(void)
{
    int i;
    int num_rules;
    MemoryContext old_cxt;

    /* Already initialized */
    if (g_pat_leaf1 != NULL)
        return;

    /*
     * Allocate pattern objects in TopMemoryContext so they survive
     * the lifecycle of the Cascades memo context.  Without this,
     * g_pat_leaf1 becomes a dangling pointer when the memo context
     * is deleted, and the idempotency guard above sees != NULL,
     * causing use-after-free on subsequent planner calls.
     */
    old_cxt = MemoryContextSwitchTo(TopMemoryContext);

    /* Create shared leaf patterns */
    g_pat_leaf1 = pg_pattern_leaf();
    g_pat_leaf2 = pg_pattern_leaf();

    /* C2: JoinAssociativity — Join(Join(Leaf, Leaf), Leaf) */
    g_pat_join_join_leaf_leaf_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_JOIN,
        list_make2(
            pg_pattern_tree(PG_CASCADES_LOGICAL_JOIN,
                list_make2(pg_pattern_leaf(), pg_pattern_leaf())),
            pg_pattern_leaf()));

    /* C3: JoinLeftAsscom — Join(Leaf, Join(Leaf, Leaf)) */
    g_pat_join_leaf_join_leaf_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_JOIN,
        list_make2(
            pg_pattern_leaf(),
            pg_pattern_tree(PG_CASCADES_LOGICAL_JOIN,
                list_make2(pg_pattern_leaf(), pg_pattern_leaf()))));

    /* A2: PushDownPredicateJoin — Filter(Join(Leaf, Leaf)) */
    g_pat_filter_join_leaf_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_FILTER,
        list_make1(
            pg_pattern_tree(PG_CASCADES_LOGICAL_JOIN,
                list_make2(pg_pattern_leaf(), pg_pattern_leaf()))));

    /* A4: PushDownPredicateProject — Filter(Project(Leaf)) */
    g_pat_filter_project_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_FILTER,
        list_make1(
            pg_pattern_tree(PG_CASCADES_LOGICAL_PROJECT,
                list_make1(pg_pattern_leaf()))));

    /* D1: MergeLimitWithSort — Limit(Sort(Leaf)) */
    g_pat_limit_sort_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_LIMIT,
        list_make1(
            pg_pattern_tree(PG_CASCADES_LOGICAL_SORT,
                list_make1(pg_pattern_leaf()))));

    /* D2: PushDownLimitJoin — Limit(Join(Leaf, Leaf)) */
    g_pat_limit_join_leaf_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_LIMIT,
        list_make1(
            pg_pattern_tree(PG_CASCADES_LOGICAL_JOIN,
                list_make2(pg_pattern_leaf(), pg_pattern_leaf()))));

    /* F2: MergeTwoAgg — Agg(Agg(Leaf)) */
    g_pat_agg_agg_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_AGG,
        list_make1(
            pg_pattern_tree(PG_CASCADES_LOGICAL_AGG,
                list_make1(pg_pattern_leaf()))));

    /* F3: PushDownAggLimit — Agg(Limit(Leaf)) */
    g_pat_agg_limit_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_AGG,
        list_make1(
            pg_pattern_tree(PG_CASCADES_LOGICAL_LIMIT,
                list_make1(pg_pattern_leaf()))));

    /* G4: MergeJoinWithChildProject — Join(Project(Leaf), Project(Leaf)) */
    g_pat_join_project_leaf_project_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_JOIN,
        list_make2(
            pg_pattern_tree(PG_CASCADES_LOGICAL_PROJECT,
                list_make1(pg_pattern_leaf())),
            pg_pattern_tree(PG_CASCADES_LOGICAL_PROJECT,
                list_make1(pg_pattern_leaf()))));

    /* H2: MergeProjectWithChild — Project(Project(Leaf)) */
    g_pat_project_project_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_PROJECT,
        list_make1(
            pg_pattern_tree(PG_CASCADES_LOGICAL_PROJECT,
                list_make1(pg_pattern_leaf()))));

    /* H4: MergeFilterWithJoin — Join(Filter(Leaf), Leaf) */
    g_pat_join_filter_leaf_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_JOIN,
        list_make2(
            pg_pattern_tree(PG_CASCADES_LOGICAL_FILTER,
                list_make1(pg_pattern_leaf())),
            pg_pattern_leaf()));

    /* Also create simple Join(Leaf, Leaf) for JoinCommutativity */
    g_pat_join_leaf_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_JOIN,
        list_make2(pg_pattern_leaf(), pg_pattern_leaf()));

    /*
     * Assign patterns to rule entries.
     * We modify the static arrays directly (safe: patterns are also static).
     */

    /* Phase 3: JoinCommutativity */
    g_trans_rules_phase3[0].pattern = g_pat_join_leaf_leaf;

    /* Phase 1 impl rules: simple patterns to activate pg_pattern_match_full */
    {
        int j;
        for (j = 0; g_impl_rules_phase1[j].name != NULL; j++)
        {
            PgRule *rule = &g_impl_rules_phase1[j];
            if (strcmp(rule->name, "LogicalProject->PhysicalProject") == 0)
                rule->pattern = g_pat_leaf1;
            else if (strcmp(rule->name, "LogicalAgg->PhysicalHashAgg") == 0)
                rule->pattern = g_pat_leaf2;
        }
    }

    /* Phase 2 join impl rules: multi-node Join(Leaf,Leaf) pattern */
    {
        int j;
        for (j = 0; g_impl_rules_phase2_join[j].name != NULL; j++)
            g_impl_rules_phase2_join[j].pattern = g_pat_join_leaf_leaf;
    }

    /* Phase 5: multi-node rules */
    num_rules = sizeof(g_trans_rules_phase5) / sizeof(PgRule) - 1;
    for (i = 0; i < num_rules; i++)
    {
        PgRule *rule = &g_trans_rules_phase5[i];

        if (strcmp(rule->name, "JoinAssociativity") == 0)
            rule->pattern = g_pat_join_join_leaf_leaf_leaf;
        else if (strcmp(rule->name, "JoinLeftAsscom") == 0)
            rule->pattern = g_pat_join_leaf_join_leaf_leaf;
        else if (strcmp(rule->name, "PushDownPredicateJoin") == 0)
            rule->pattern = g_pat_filter_join_leaf_leaf;
        else if (strcmp(rule->name, "PushDownPredicateProject") == 0)
            rule->pattern = g_pat_filter_project_leaf;
        else if (strcmp(rule->name, "MergeLimitWithSort") == 0)
            rule->pattern = g_pat_limit_sort_leaf;
        else if (strcmp(rule->name, "PushDownLimitJoin") == 0)
            rule->pattern = g_pat_limit_join_leaf_leaf;
        else if (strcmp(rule->name, "MergeTwoAgg") == 0)
            rule->pattern = g_pat_agg_agg_leaf;
        else if (strcmp(rule->name, "PushDownAggLimit") == 0)
            rule->pattern = g_pat_agg_limit_leaf;
        else if (strcmp(rule->name, "MergeJoinWithChildProj") == 0)
            rule->pattern = g_pat_join_project_leaf_project_leaf;
        else if (strcmp(rule->name, "MergeProjectWithChild") == 0)
            rule->pattern = g_pat_project_project_leaf;
        else if (strcmp(rule->name, "MergeFilterWithJoin") == 0)
            rule->pattern = g_pat_join_filter_leaf_leaf;
        /* Other rules use from_op (pattern stays NULL) */
    }

    MemoryContextSwitchTo(old_cxt);
}

/* ========================================================================
 * Phase 4: Combination Rules
 *
 *   CombinationRule groups related rules into execution units
 *   for staged rewrite in the pipeline.
 * ======================================================================== */

static PgCombinationRule g_combination_rules[] = {
    /*
     * GP_PUSH_DOWN_PREDICATE: Predicate pushdown group.
     *   PushDownPredicateScan, PushDownPredicateJoin,
     *   PushDownPredicateProject, PushDownPredicateAgg,
         */
    {"GP_PUSH_DOWN_PREDICATE",
     NULL,  /* rule_ids computed from name lookup in rewrite */
     true   /* iterate until convergence */
    },

    /*
     * GP_PRUNE_COLUMNS: Column pruning group.
     *   PruneScanColumns, PruneJoinColumns, PruneAggColumns,
     *   PruneProjectColumns, PruneSortColumns
     */
    {"GP_PRUNE_COLUMNS", NULL, true},

    /*
     * GP_JOIN_REORDER: Join reorder group.
     *   JoinCommutativity, JoinAssociativity, JoinLeftAsscom
     */
    /* Phase 7: re-enabled with safety guards */
    {"GP_JOIN_REORDER", NULL, true},

    /*
     * GP_PRUNE_EMPTY: Empty set pruning group.
     *   PruneEmptyJoin, PruneEmptyScan, PruneEmptyUnion
     */
    {"GP_PRUNE_EMPTY", NULL, true},

    {NULL, NULL, false}  /* sentinel */
};

/*
 * pg_cascades_init_combination_rules:
 *   No longer needed.  Combination rules (g_combination_rules[]) are
 *   consumed directly by pg_cascades_logical_rewrite() via
 *   pg_cascades_get_combination_rules(), which builds per-combo rule
 *   lists from pg_rewrite_lookup_transform() at each call site.
 *   This function is retained as a no-op for API compatibility.
 */
void
pg_cascades_init_combination_rules(void)
{
    /* No dynamic initialization needed — combination rules are
     * driven by pg_cascades_logical_rewrite() at runtime. */
}

PgCombinationRule *
pg_cascades_get_combination_rules(int *num_rules)
{
    *num_rules = (int)(sizeof(g_combination_rules) / sizeof(PgCombinationRule)) - 1;
    return g_combination_rules;
}

/* ========================================================================
 * Phase 4: Path Generation Join Implementation Rules
 *
 *   These rules call make_join_rel() internally to generate physical
 *   join paths on-demand during Memo search, replacing the "import all
 *   paths upfront" approach.
 *
 *   First version: INNER JOIN only.  Relies on prep->final_rel for
 *   base relations having valid RelOptInfo with pathlist populated.
 * ======================================================================== */

static RelOptInfo *
pg_rule_join_get_child_rel(PgPlannerCascadesContext *ctx, PgMemoGroup *child_grp)
{
    /* Direct mapping: base rel group */
    if (child_grp->rel != NULL)
        return child_grp->rel;

    /* Try group_to_rel for join groups */
    return pg_cascades_group_to_rel(ctx, child_grp);
}

/*
 * Phase 4 HashJoin: LogicalJoin → PhysicalHashJoin via make_join_rel
 */
static List *
pg_rule_join_to_hashjoin_phase4(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgJoinPrivate *join_priv;
    PgMemoGroup *outer_grp, *inner_grp;
    RelOptInfo *outer_rel, *inner_rel, *joinrel;
    List *result = NIL;
    ListCell *lc;

    if (expr == NULL || expr->inputs == NIL)
        return NIL;

    join_priv = (PgJoinPrivate *) expr->op_private;
    if (join_priv == NULL || join_priv->jointype != JOIN_INNER)
        return NIL;

    outer_grp = (PgMemoGroup *) linitial(expr->inputs);
    inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

    outer_rel = pg_rule_join_get_child_rel(ctx, outer_grp);
    inner_rel = pg_rule_join_get_child_rel(ctx, inner_grp);

    if (outer_rel == NULL || inner_rel == NULL)
        return NIL;

    joinrel = make_join_rel(ctx->root, outer_rel, inner_rel);
    if (joinrel == NULL)
        return NIL;

    set_cheapest(joinrel);

    foreach(lc, joinrel->pathlist)
    {
        Path *path = (Path *) lfirst(lc);
        if (path->pathtype == T_HashJoin)
        {
            PgGroupExpr *phys = pg_memo_new_group_expr(ctx,
                                        PG_CASCADES_PHYSICAL_HASHJOIN);
            phys->mode = PG_PHYS_EXPR_IMPORTED_PATH;
            phys->op_private = path;
            phys->inputs = NIL;
            result = lappend(result, phys);
        }
    }
    return result;
}

/*
 * Phase 4 NestLoop: LogicalJoin → PhysicalNestLoop via make_join_rel
 */
static List *
pg_rule_join_to_nestloop_phase4(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgJoinPrivate *join_priv;
    PgMemoGroup *outer_grp, *inner_grp;
    RelOptInfo *outer_rel, *inner_rel, *joinrel;
    List *result = NIL;
    ListCell *lc;

    if (expr == NULL || expr->inputs == NIL)
        return NIL;

    join_priv = (PgJoinPrivate *) expr->op_private;
    if (join_priv == NULL || join_priv->jointype != JOIN_INNER)
        return NIL;

    outer_grp = (PgMemoGroup *) linitial(expr->inputs);
    inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

    outer_rel = pg_rule_join_get_child_rel(ctx, outer_grp);
    inner_rel = pg_rule_join_get_child_rel(ctx, inner_grp);

    if (outer_rel == NULL || inner_rel == NULL)
        return NIL;

    joinrel = make_join_rel(ctx->root, outer_rel, inner_rel);
    if (joinrel == NULL)
        return NIL;

    set_cheapest(joinrel);

    foreach(lc, joinrel->pathlist)
    {
        Path *path = (Path *) lfirst(lc);
        if (path->pathtype == T_NestLoop)
        {
            PgGroupExpr *phys = pg_memo_new_group_expr(ctx,
                                        PG_CASCADES_PHYSICAL_NESTLOOP);
            phys->mode = PG_PHYS_EXPR_IMPORTED_PATH;
            phys->op_private = path;
            phys->inputs = NIL;
            result = lappend(result, phys);
        }
    }
    return result;
}

/*
 * Phase 4 MergeJoin: LogicalJoin → PhysicalMergeJoin via make_join_rel
 */
static List *
pg_rule_join_to_mergejoin_phase4(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgJoinPrivate *join_priv;
    PgMemoGroup *outer_grp, *inner_grp;
    RelOptInfo *outer_rel, *inner_rel, *joinrel;
    List *result = NIL;
    ListCell *lc;

    if (expr == NULL || expr->inputs == NIL)
        return NIL;

    join_priv = (PgJoinPrivate *) expr->op_private;
    if (join_priv == NULL || join_priv->jointype != JOIN_INNER)
        return NIL;

    outer_grp = (PgMemoGroup *) linitial(expr->inputs);
    inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

    outer_rel = pg_rule_join_get_child_rel(ctx, outer_grp);
    inner_rel = pg_rule_join_get_child_rel(ctx, inner_grp);

    if (outer_rel == NULL || inner_rel == NULL)
        return NIL;

    joinrel = make_join_rel(ctx->root, outer_rel, inner_rel);
    if (joinrel == NULL)
        return NIL;

    set_cheapest(joinrel);

    foreach(lc, joinrel->pathlist)
    {
        Path *path = (Path *) lfirst(lc);
        if (path->pathtype == T_MergeJoin)
        {
            PgGroupExpr *phys = pg_memo_new_group_expr(ctx,
                                        PG_CASCADES_PHYSICAL_MERGEJOIN);
            phys->mode = PG_PHYS_EXPR_IMPORTED_PATH;
            phys->op_private = path;
            phys->inputs = NIL;
            result = lappend(result, phys);
        }
    }
    return result;
}
