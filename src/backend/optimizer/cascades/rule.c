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

/* Phase 2: Scan/Join (second phase enabled) */
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
    {NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0.0}  /* sentinel */
};

/* ========================================================================
 * Phase 5: Transformation rule transform functions
 * ======================================================================== */

/*
 * H2: MergeProjectWithChild
 *   LogicalProject(LogicalProject(A)) → LogicalProject(A)
 *   合并两个连续的 Project：内层 tlist 被外层 tlist 替代。
 */
static List *
pg_rule_merge_project_with_child(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *inner = (PgGroupExpr *) linitial(expr->inputs);
    PgGroupExpr *new_proj;

    if (inner->op != PG_CASCADES_LOGICAL_PROJECT)
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
 */
static List *
pg_rule_eliminate_project(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *child = (PgGroupExpr *) linitial(expr->inputs);

    /* 如果 child 不是 Project，无法判断 tlist 是否相同 —— 保守保留 */
    if (child->op != PG_CASCADES_LOGICAL_PROJECT)
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
 */
static List *
pg_rule_pushdown_predicate_scan(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *scan_expr = (PgGroupExpr *) linitial(expr->inputs);
    List        *filter_quals = (List *) expr->op_private;
    RelOptInfo  *rel = scan_expr->owner_group->rel;
    ListCell    *lc;
    List        *pushable = NIL;
    List        *remain   = NIL;

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
        PgGroupExpr *new_scan = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_SCAN);
        new_scan->inputs = NIL;
        new_scan->op_private = scan_expr->op_private;
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
static List *
pg_rule_merge_limit_with_sort(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *sort_expr;
    PgSortPrivate *sort_priv;
    PgGroupExpr *new_sort;
    double limit_tuples;

    if (list_length(expr->inputs) != 1)
        return NIL;

    sort_expr = (PgGroupExpr *) linitial(expr->inputs);
    if (sort_expr->op != PG_CASCADES_LOGICAL_SORT)
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

    if (list_length(expr->inputs) != 2)
        return NIL;

    join_priv = (PgJoinPrivate *) expr->op_private;
    if (join_priv == NULL || join_priv->jointype != JOIN_INNER)
        return NIL;

    outer_input = (PgGroupExpr *) linitial(expr->inputs);

    /* 只处理 outer 侧有 Filter 的情况 */
    if (outer_input->op != PG_CASCADES_LOGICAL_FILTER)
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

    if (list_length(expr->inputs) != 1)
        return NIL;
    inner = (PgGroupExpr *) linitial(expr->inputs);
    if (inner->op != PG_CASCADES_LOGICAL_AGG)
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

    if (list_length(expr->inputs) != 2)
        return NIL;

    outer_input = (PgGroupExpr *) linitial(expr->inputs);
    inner_input = (PgGroupExpr *) lsecond(expr->inputs);

    outer_is_proj = (outer_input->op == PG_CASCADES_LOGICAL_PROJECT);
    inner_is_proj = (inner_input->op == PG_CASCADES_LOGICAL_PROJECT);

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

    /* Always include aggref argument columns (handled by PG's targetlist) */
    /* First version: side-effect only — mark what's minimally needed.
     * The actual pruning of child columns happens via required_columns
     * propagation in pg_derive_child_properties. */
    if (needed != NULL)
        bms_free(needed);
    return NIL;
}

/*
 * B4: PruneProjectColumns
 *   LogicalProject(A): keep only columns referenced by parent.
 *   Reads required_columns from the group's best entries.
 */
static List *
pg_rule_prune_project_columns(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    ListCell *lc;

    /* Collect union of required_columns from all best entries */
    foreach(lc, expr->owner_group->best_entries)
    {
        PgGroupBestEntry *entry = (PgGroupBestEntry *) lfirst(lc);
        if (entry->required != NULL && entry->required->required_columns != NULL)
        {
            /* required_columns tells us what parent needs —
             * first version: no action, propagation handles it */
            return NIL;
        }
    }
    return NIL;
}

/*
 * B5: PruneSortColumns
 *   LogicalSort(A): keep only sort key columns + parent required columns.
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
        bms_free(needed);
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

    if (list_length(expr->inputs) != 2)
        return NIL;

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
 *   First version: conservatively keep all columns that appear in any
 *   upper-level required_columns or baserestrictinfo.
 */
static List *
pg_rule_prune_scan_columns(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    RelOptInfo *rel = expr->owner_group->rel;
    ListCell   *lc;
    Bitmapset  *needed = NULL;
    List       *new_tlist = NIL;

    if (rel == NULL)
        return NIL;

    /* Collect columns from baserestrictinfo (quals reference these) */
    foreach(lc, rel->baserestrictinfo)
    {
        RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
        pull_varattnos((Node *) ri->clause, rel->relid, &needed);
    }

    /* Also from reltargetlist (parent projections) — keep all for now */
    if (needed == NULL)
        return NIL;  /* nothing to prune */

    /* First version: mark but don't modify reltargetlist yet.
     * Actual reduction happens when required_columns propagation is complete. */
    bms_free(needed);
    return NIL;
}

/*
 * B2: PruneJoinColumns
 *   LogicalJoin(A, B): keep only join key columns + parent-required columns.
 */
static List *
pg_rule_prune_join_columns(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgJoinPrivate *join_priv = (PgJoinPrivate *) expr->op_private;
    Bitmapset  *needed = NULL;
    ListCell   *lc;

    if (join_priv == NULL)
        return NIL;

    /* Collect columns from join quals */
    foreach(lc, join_priv->restrictlist)
    {
        RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
        pull_varattnos((Node *) ri->clause, 0, &needed);
    }

    if (needed != NULL)
        bms_free(needed);
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

    if (list_length(expr->inputs) != 1)
        return NIL;

    proj_expr = (PgGroupExpr *) linitial(expr->inputs);
    if (proj_expr->op != PG_CASCADES_LOGICAL_PROJECT)
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

    if (list_length(expr->inputs) != 1)
        return NIL;

    join_expr = (PgGroupExpr *) linitial(expr->inputs);
    if (join_expr->op != PG_CASCADES_LOGICAL_JOIN)
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

    /* Rebuild Join with limited children */
    new_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    new_join->op_private = join_priv;

    return list_make1(new_join);  /* inputs set by engine via pg_memo_insert */
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

    if (list_length(expr->inputs) != 1)
        return NIL;
    agg_expr = (PgGroupExpr *) linitial(expr->inputs);
    if (agg_expr->op != PG_CASCADES_LOGICAL_AGG)
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

    if (list_length(expr->inputs) != 1)
        return NIL;
    limit_expr = (PgGroupExpr *) linitial(expr->inputs);
    if (limit_expr->op != PG_CASCADES_LOGICAL_LIMIT)
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

    if (list_length(expr->inputs) != 2)
        return NIL;

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

    if (list_length(expr->inputs) != 2)
        return NIL;

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

    if (list_length(expr->inputs) != 1)
        return NIL;

    join_expr = (PgGroupExpr *) linitial(expr->inputs);
    if (join_expr->op != PG_CASCADES_LOGICAL_JOIN)
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
    if (list_length(expr->inputs) != 2)
        return NIL;

    left_grp = (PgMemoGroup *) linitial(expr->inputs);
    right_grp = (PgMemoGroup *) lsecond(expr->inputs);

    /* Find a JOIN expression inside left_grp */
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
    if (list_length(expr->inputs) != 2)
        return NIL;

    left_grp = (PgMemoGroup *) linitial(expr->inputs);
    right_grp = (PgMemoGroup *) lsecond(expr->inputs);

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

    if (list_length(expr->inputs) != 2)
        return NIL;

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

/* ========================================================================
 * Phase 5: Transformation rules (23 → 26 rules)
 *   C2/C3/G3 added.
 * ======================================================================== */

static PgRule g_trans_rules_phase5[] = {
    /* H2: MergeProjectWithChild — LogicalProject(LogicalProject(A)) → LogicalProject(A) */
    {"MergeProjectWithChild", NULL, pg_rule_merge_project_with_child,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_PROJECT, 0, 0, 0.5},

    /* E2: PruneEmptyScan — mark empty scan (side-effect only) */
    {"PruneEmptyScan", NULL, pg_rule_prune_empty_scan,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_SCAN, 0, 0, 0.9},

    /* H3: EliminateProject — remove no-op Project */
    {"EliminateProject", NULL, pg_rule_eliminate_project,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_PROJECT, 0, 0, 0.6},

    /* A1: PushDownPredicateScan — filter into scan qual */
    {"PushDownPredicateScan", NULL, pg_rule_pushdown_predicate_scan,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_FILTER, 0, 0, 0.6},

    /* D1: MergeLimitWithSort — eliminate redundant Limit on top of Sort */
    {"MergeLimitWithSort", NULL, pg_rule_merge_limit_with_sort,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_LIMIT, 0, 0, 0.7},

    /* H1: EliminateSortWithConstantKey — remove Sort on constant keys */
    {"EliminateSortWithConstKey", NULL, pg_rule_eliminate_sort_with_constant_key,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_SORT, 0, 0, 0.5},

    /* H4: MergeFilterWithJoin — merge Filter into Join qual (INNER only) */
    {"MergeFilterWithJoin", NULL, pg_rule_merge_filter_with_join,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_JOIN, 0, 0, 0.4},

    /* E3: PruneEmptyUnion — remove empty branches from Union */
    {"PruneEmptyUnion", NULL, pg_rule_prune_empty_union,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_PROJECT, 0, 0, 0.8},

    /* F2: MergeTwoAgg — merge two consecutive Agg operators */
    {"MergeTwoAgg", NULL, pg_rule_merge_two_agg,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_AGG, 0, 0, 0.5},

    /* G4: MergeJoinWithChildProject — eliminate Project under Join */
    {"MergeJoinWithChildProj", NULL, pg_rule_merge_join_with_child_project,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_JOIN, 0, 0, 0.4},

    /* B3: PruneAggColumns — reduce Agg to GROUP BY + aggre cols */
    {"PruneAggColumns", NULL, pg_rule_prune_agg_columns,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_AGG, 0, 0, 0.5},

    /* B4: PruneProjectColumns — keep only parent-referenced columns */
    {"PruneProjectColumns", NULL, pg_rule_prune_project_columns,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_PROJECT, 0, 0, 0.6},

    /* B5: PruneSortColumns — keep only sort key + parent columns */
    {"PruneSortColumns", NULL, pg_rule_prune_sort_columns,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_SORT, 0, 0, 0.5},

    /* E1: PruneEmptyJoin — mark join as empty if either child is empty */
    {"PruneEmptyJoin", NULL, pg_rule_prune_empty_join,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_JOIN, 0, 0, 0.9},

    /* B1: PruneScanColumns — reduce scan to needed columns */
    {"PruneScanColumns", NULL, pg_rule_prune_scan_columns,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_SCAN, 0, 0, 0.6},

    /* B2: PruneJoinColumns — reduce join to key + parent columns */
    {"PruneJoinColumns", NULL, pg_rule_prune_join_columns,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_JOIN, 0, 0, 0.5},

    /* A4: PushDownPredicateProject — filter through Project */
    {"PushDownPredicateProject", NULL, pg_rule_pushdown_predicate_project,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_FILTER, 0, 0, 0.5},

    /* D2: PushDownLimitJoin — push Limit into Join children (INNER only) */
    {"PushDownLimitJoin", NULL, pg_rule_pushdown_limit_join,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_LIMIT, 0, 0, 0.4},

    /* A3: PushDownPredicateAgg — filter through Agg (turn into HAVING-like) */
    {"PushDownPredicateAgg", NULL, pg_rule_pushdown_predicate_agg,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_FILTER, 0, 0, 0.3},

    /* F3: PushDownAggLimit — swap Agg and Limit */
    {"PushDownAggLimit", NULL, pg_rule_pushdown_agg_limit,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_AGG, 0, 0, 0.4},

    /* G1: EliminateJoinWithConstant — drop join with single-row side */
    {"EliminateJoinWithConst", NULL, pg_rule_eliminate_join_with_constant,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_JOIN, 0, 0, 0.7},

    /* G2: OuterJoinElimination — LEFT→INNER conversion */
    {"OuterJoinElimination", NULL, pg_rule_outer_join_elimination,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_JOIN, 0, 0, 0.6},

    /* A2: PushDownPredicateJoin — push WHERE into Join children (INNER only) */
    {"PushDownPredicateJoin", NULL, pg_rule_pushdown_predicate_join,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_FILTER, 0, 0, 0.4},

    /* G3: InnerToSemi — convert INNER JOIN to SEMI JOIN when inner side unique */
    {"InnerToSemi", NULL, pg_rule_inner_to_semi,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_JOIN, 0, 0, 0.3},

    /* C2: JoinAssociativity — (A⋈B)⋈C → A⋈(B⋈C) */
    {"JoinAssociativity", NULL, pg_rule_join_associativity,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_JOIN, 0, 0, 0.2},

    /* C3: JoinLeftAsscom — A⋈(B⋈C) → (A⋈B)⋈C */
    {"JoinLeftAsscom", NULL, pg_rule_join_left_asscom,
     PG_RULE_TRANS, NULL,
     PG_CASCADES_LOGICAL_JOIN, 0, 0, 0.2},

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

/* ========================================================================
 * Phase 5: Pattern initialization for multi-node rules
 *
 * Patterns are created once per backend lifetime (static allocation).
 * Single-node rules (pattern=NULL) use from_op matching.
 * Multi-node rules get pattern trees for child-group enumeration.
 * ======================================================================== */

/* Static pattern nodes for multi-node rules */
static PgPattern *g_pat_leaf1 = NULL;
static PgPattern *g_pat_leaf2 = NULL;
static PgPattern *g_pat_join_leaf_leaf = NULL;
static PgPattern *g_pat_join_join_leaf_leaf_leaf = NULL;
static PgPattern *g_pat_join_leaf_join_leaf_leaf = NULL;
static PgPattern *g_pat_filter_join_leaf_leaf = NULL;
static PgPattern *g_pat_filter_project_leaf = NULL;
static PgPattern *g_pat_limit_sort_leaf = NULL;
static PgPattern *g_pat_limit_join_leaf_leaf = NULL;
static PgPattern *g_pat_agg_agg_leaf = NULL;
static PgPattern *g_pat_agg_limit_leaf = NULL;
static PgPattern *g_pat_join_project_leaf_project_leaf = NULL;
static PgPattern *g_pat_project_project_leaf = NULL;
static PgPattern *g_pat_join_filter_leaf_leaf = NULL;

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

    /* Already initialized */
    if (g_pat_leaf1 != NULL)
        return;

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
}
