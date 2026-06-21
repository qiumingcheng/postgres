/*-------------------------------------------------------------------------
 * pg_adapter.c
 *    PG 适配层：在 Cascades 世界和 PG 世界之间转换。
 *    - build logical root from PG structures
 *    - Path type to Cascades op kind mapping
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "optimizer/pathnode.h"
#include "nodes/primnodes.h"

/* ========================================================================
 * Path 类型 → Cascades Op Kind
 * ======================================================================== */

PgCascadesOpKind
pg_cascades_pathtype_to_opkind(NodeTag pathtype)
{
    switch (pathtype)
    {
        case T_SeqScan:       return PG_CASCADES_PHYSICAL_SEQSCAN;
        case T_IndexScan:     return PG_CASCADES_PHYSICAL_INDEXSCAN;
        case T_IndexOnlyScan: return PG_CASCADES_PHYSICAL_INDEXSCAN;
        case T_BitmapHeapScan:return PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN;
        case T_NestLoop:      return PG_CASCADES_PHYSICAL_NESTLOOP;
        case T_HashJoin:      return PG_CASCADES_PHYSICAL_HASHJOIN;
        case T_MergeJoin:     return PG_CASCADES_PHYSICAL_MERGEJOIN;
        default:
            return -1;  /* sentinel: unsupported path type */
    }
}

/* ========================================================================
 * 构建 Logical Root Tree（Path 导入模式）
 * ======================================================================== */

/*
 * pg_cascades_build_memo_path_import:
 *   将 PG 生成的 final_rel->pathlist 导入 Memo 作为 lower physical candidates，
 *   并在其上构建 upper logical ops 树。
 *
 * 这是第一版的核心 Memo 构建算法。
 */
static PgMemo *
pg_cascades_build_memo_path_import(PgPlannerCascadesContext *ctx)
{
    PgMemo     *memo;
    PgMemoGroup *lower_group;
    PgMemoGroup *current_group;
    ListCell   *lc;

    /* Create Memo */
    {
        MemoryContext old_cxt = MemoryContextSwitchTo(ctx->memo_cxt);

        memo = (PgMemo *) palloc0(sizeof(PgMemo));
        memo->context = ctx->memo_cxt;
        memo->groups = NIL;
        memo->group_expr_table = NULL;
        ctx->memo = memo;

        MemoryContextSwitchTo(old_cxt);
    }

    /* === Step 1: create lower group from PG paths === */
    lower_group = pg_memo_new_group(ctx);

    if (ctx->prep->final_rel != NULL)
    {
        foreach(lc, ctx->prep->final_rel->pathlist)
        {
            Path       *path = (Path *) lfirst(lc);
            PgGroupExpr *phys_expr;
            PgCascadesOpKind op;

            op = pg_cascades_pathtype_to_opkind(path->pathtype);
            if ((int) op < 0)
                continue;  /* skip unsupported path types */

            phys_expr = pg_memo_new_group_expr(ctx, op);
            phys_expr->mode = PG_PHYS_EXPR_IMPORTED_PATH;
            phys_expr->op_private = path;
            phys_expr->inputs = NIL;

            pg_memo_add_physical_expr(lower_group, phys_expr);
        }

        lower_group->rows = ctx->prep->final_rel->rows;
        lower_group->width = ctx->prep->final_rel->width;
    }

    /* === Step 2: wrap upper logical ops === */
    current_group = lower_group;

    /* LogicalProject: use sub_tlist (no Aggrefs) below any upper ops */
    {
        PgGroupExpr *proj = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_PROJECT);
        proj->inputs = list_make1(current_group);
        proj->op_private = ctx->upper->sub_tlist;
        current_group = pg_memo_insert_expression(ctx, memo, proj, NULL);
    }

    /* LogicalAggregation（如果有 GROUP BY 或 hasAggs）*/
    if (ctx->upper->groupClause != NIL || ctx->upper->hasAggs)
    {
        PgGroupExpr *agg = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_AGG);
        agg->inputs = list_make1(current_group);
        agg->op_private = ctx->upper;  /* tlist/having/agg_costs all in upper */
        current_group = pg_memo_insert_expression(ctx, memo, agg, NULL);
    }

    /* LogicalDistinct */
    if (ctx->upper->distinctClause != NIL)
    {
        PgGroupExpr *dist = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_DISTINCT);
        dist->inputs = list_make1(current_group);
        dist->op_private = ctx->upper;
        current_group = pg_memo_insert_expression(ctx, memo, dist, NULL);
    }

    /* LogicalSort */
    if (ctx->upper->sortClause != NIL)
    {
        PgGroupExpr *sort = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_SORT);
        sort->inputs = list_make1(current_group);
        sort->op_private = ctx->upper;
        current_group = pg_memo_insert_expression(ctx, memo, sort, NULL);
    }

    /* LogicalLimit */
    if (ctx->root->parse->limitCount || ctx->root->parse->limitOffset)
    {
        PgGroupExpr *limit = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_LIMIT);
        limit->inputs = list_make1(current_group);
        limit->op_private = ctx->upper;
        current_group = pg_memo_insert_expression(ctx, memo, limit, NULL);
    }

    memo->root_group = current_group;
    return memo;
}

/* ========================================================================
 * Phase 3: Join Search Memo Builder
 * ======================================================================== */

/*
 * pg_cascades_find_joinrel_by_relids:
 *   从 root->join_rel_list 中查找包含指定 relids 集合的 joinrel。
 */
static RelOptInfo *
pg_cascades_find_joinrel_by_relids(PlannerInfo *root, Relids relids)
{
    ListCell *lc;

    foreach(lc, root->join_rel_list)
    {
        RelOptInfo *joinrel = (RelOptInfo *) lfirst(lc);
        if (bms_equal(joinrel->relids, relids))
            return joinrel;
    }
    return NULL;
}

/*
 * pg_cascades_import_paths_to_group:
 *   将 RelOptInfo 的 pathlist 中的 Path 导入为 Memo group 的 physical expr。
 */
static void
pg_cascades_import_paths_to_group(PgPlannerCascadesContext *ctx,
                                   PgMemoGroup *group, RelOptInfo *rel)
{
    ListCell *lc;

    if (rel == NULL)
        return;

    foreach(lc, rel->pathlist)
    {
        Path       *path = (Path *) lfirst(lc);
        PgGroupExpr *phys_expr;
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

    group->rows = rel->rows;
    group->width = rel->width;
    group->rel = rel;
}

/*
 * pg_cascades_build_join_group:
 *   从 joinlist 的子树递归构建 Memo group。
 *
 *   joinlist 是 deconstruct_jointree 的输出，包含 RangeTblRef 和 sub-List。
 *   返回该子树对应的 PgMemoGroup *。
 */
static PgMemoGroup *
pg_cascades_build_join_group(PgPlannerCascadesContext *ctx,
                              PgMemo *memo, List *joinlist)
{
    int nkids = list_length(joinlist);
    PgMemoGroup *group;

    if (nkids == 0)
        return NULL;

    if (nkids == 1)
    {
        Node *node = (Node *) linitial(joinlist);

        /* Base relation: RangeTblRef */
        if (IsA(node, RangeTblRef))
        {
            RangeTblRef *rtr = (RangeTblRef *) node;
            RelOptInfo *rel = ctx->root->simple_rel_array[rtr->rtindex];

            if (rel == NULL)
                return NULL;

            /* Create group and import scan paths */
            group = pg_memo_new_group(ctx);
            pg_cascades_import_paths_to_group(ctx, group, rel);
            return group;
        }

        /* Nested sub-joinlist */
        if (IsA(node, List))
            return pg_cascades_build_join_group(ctx, memo, (List *) node);
    }

    /* nkids >= 2: build LogicalJoin tree (left-deep) */
    {
        ListCell *jc;

        /* Safety: verify all entries are RangeTblRef or List */
        foreach(jc, joinlist)
        {
            Node *n = (Node *) lfirst(jc);
            if (!IsA(n, RangeTblRef) && !IsA(n, List))
            {
                if (cascades_planner_debug)
                    elog(NOTICE, "Cascades Phase3: complex joinlist node type %d, bailing",
                         nodeTag(n));
                return NULL;
            }
        }

        /* Build left child from first nkids-1 items, right child from last item */
        List *left_list = list_copy(joinlist);
        PgMemoGroup *left_group;
        PgMemoGroup *right_group;
        PgGroupExpr *join_expr;
        RelOptInfo *joinrel;
        Relids joinrelids;

        /* Remove last element for left */
        left_list = list_truncate(left_list, nkids - 1);
        left_group = pg_cascades_build_join_group(ctx, memo, left_list);
        list_free(left_list);

        /* Right child: last element */
        {
            Node *last = (Node *) list_nth(joinlist, nkids - 1);
            List *right_list = list_make1(last);
            right_group = pg_cascades_build_join_group(ctx, memo, right_list);
            list_free(right_list);
        }

        if (left_group == NULL || right_group == NULL)
            return NULL;

        /* Compute the joinrelids for this join */
        if (left_group->rel != NULL && right_group->rel != NULL)
            joinrelids = bms_union(left_group->rel->relids, right_group->rel->relids);
        else
            return NULL;

        /* Look up the PG joinrel */
        joinrel = pg_cascades_find_joinrel_by_relids(ctx->root, joinrelids);
        bms_free(joinrelids);

        /* Create LogicalJoin group + import join paths */
        group = pg_memo_new_group(ctx);

        join_expr = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
        join_expr->inputs = list_make2(left_group, right_group);
        join_expr->op_private = NULL;  /* Join info stored elsewhere */
        pg_memo_add_logical_expr(group, join_expr);

        /* Import join paths from the PG joinrel */
        if (joinrel != NULL)
            pg_cascades_import_paths_to_group(ctx, group, joinrel);

        return group;
    }
}

/*
 * pg_cascades_build_memo_phase3:
 *   Phase 3 Memo 构建：base rel groups + LogicalJoin tree + upper ops。
 */
static PgMemo *
pg_cascades_build_memo_phase3(PgPlannerCascadesContext *ctx)
{
    PgMemo     *memo;
    PgMemoGroup *lower_group;
    PgMemoGroup *current_group;
    MemoryContext old_cxt;

    /* Create Memo */
    old_cxt = MemoryContextSwitchTo(ctx->memo_cxt);

    memo = (PgMemo *) palloc0(sizeof(PgMemo));
    memo->context = ctx->memo_cxt;
    memo->groups = NIL;
    memo->group_expr_table = NULL;
    ctx->memo = memo;

    MemoryContextSwitchTo(old_cxt);

    /* === Step 1: Build join tree from joinlist === */
    lower_group = pg_cascades_build_join_group(ctx, memo, ctx->prep->joinlist);

    if (lower_group == NULL)
    {
        if (cascades_planner_debug)
            elog(NOTICE, "Cascades Phase3: failed to build join tree");
        return NULL;
    }

    /* === Step 2: wrap upper logical ops === */
    current_group = lower_group;

    /* LogicalProject */
    {
        PgGroupExpr *proj = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_PROJECT);
        proj->inputs = list_make1(current_group);
        proj->op_private = ctx->upper->sub_tlist;
        current_group = pg_memo_insert_expression(ctx, memo, proj, NULL);
    }

    /* LogicalAggregation */
    if (ctx->upper->groupClause != NIL || ctx->upper->hasAggs)
    {
        PgGroupExpr *agg = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_AGG);
        agg->inputs = list_make1(current_group);
        agg->op_private = ctx->upper;
        current_group = pg_memo_insert_expression(ctx, memo, agg, NULL);
    }

    /* LogicalDistinct */
    if (ctx->upper->distinctClause != NIL)
    {
        PgGroupExpr *dist = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_DISTINCT);
        dist->inputs = list_make1(current_group);
        dist->op_private = ctx->upper;
        current_group = pg_memo_insert_expression(ctx, memo, dist, NULL);
    }

    /* LogicalSort */
    if (ctx->upper->sortClause != NIL)
    {
        PgGroupExpr *sort = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_SORT);
        sort->inputs = list_make1(current_group);
        sort->op_private = ctx->upper;
        current_group = pg_memo_insert_expression(ctx, memo, sort, NULL);
    }

    /* LogicalLimit */
    if (ctx->root->parse->limitCount || ctx->root->parse->limitOffset)
    {
        PgGroupExpr *limit = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_LIMIT);
        limit->inputs = list_make1(current_group);
        limit->op_private = ctx->upper;
        current_group = pg_memo_insert_expression(ctx, memo, limit, NULL);
    }

    memo->root_group = current_group;
    return memo;
}

/* ========================================================================
 * 主入口：构建完整 logical tree + Memo
 * ======================================================================== */

PgGroupExpr *
pg_cascades_build_logical_root(PgPlannerCascadesContext *ctx)
{
    /*
     * Phase 3: Join search mode.
     *   Try to build base rel groups + LogicalJoin tree + upper ops.
     *   If the joinlist structure is too complex (e.g., RIGHT JOIN
     *   producing nested lists we can't handle), fall back to
     *   Phase 2: import final_rel->pathlist directly.
     */
    ctx->memo = pg_cascades_build_memo_phase3(ctx);
    if (ctx->memo == NULL)
    {
        /* Phase 3 failed — fallback to Phase 2 path import */
        if (cascades_planner_debug)
            elog(NOTICE, "Cascades: Phase3 join tree failed, falling back to Phase2 path import");
        ctx->memo = pg_cascades_build_memo_path_import(ctx);
    }
    return NULL;
}
