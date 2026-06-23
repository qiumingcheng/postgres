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
 * pg_cascades_build_initial_tree:
 *    Phase 4: Build a standalone OptExpression tree from the PG parse tree.
 *    This tree is NOT inserted into Memo — it's used for pre-Memo rewrite.
 *
 *    Returns the root PgGroupExpr (logical tree).
 *    After rewrite, the caller converts it to Memo via pg_memo_init().
 *
 *    Structure:  LogicalLimit → LogicalSort → LogicalDistinct →
 *                LogicalAgg → LogicalProject → LogicalJoin → LogicalScan
 *
 *    First version: use path-import approach (make_one_rel already done),
 *    wrap the lower group with upper logical ops as a standalone tree.
 */
PgGroupExpr *
pg_cascades_build_initial_tree(PgPlannerCascadesContext *ctx)
{
    PgCascadesUpperInfo *upper = ctx->upper;
    PgGroupExpr *current = NULL;

    /*
     * Build the lower Scan node: represents the entire FROM/JOIN/WHERE result.
     * Use a dummy LogicalScan whose op_private is the final_rel pointer
     * — rewrite rules use this to access relation info.
     */
    current = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_SCAN);
    current->inputs = NIL;
    current->op_private = ctx->prep->final_rel;

    /* LogicalProject */
    {
        PgGroupExpr *proj = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_PROJECT);
        proj->inputs = list_make1(current);
        proj->op_private = upper->tlist;
        current = proj;
    }

    /* LogicalAggregation (if applicable) */
    if (upper->groupClause != NIL || upper->hasAggs)
    {
        PgGroupExpr *agg = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_AGG);
        agg->inputs = list_make1(current);
        agg->op_private = upper;
        current = agg;
    }

    /* LogicalDistinct (if applicable) */
    if (upper->distinctClause != NIL)
    {
        PgGroupExpr *dist = pg_memo_new_group_expr(ctx,
                                PG_CASCADES_LOGICAL_DISTINCT);
        dist->inputs = list_make1(current);
        dist->op_private = upper->distinctClause;
        current = dist;
    }

    /* LogicalSort (if applicable) */
    if (upper->sortClause != NIL)
    {
        PgGroupExpr *sort = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_SORT);
        sort->inputs = list_make1(current);
        sort->op_private = upper->sort_pathkeys;
        current = sort;
    }

    /* LogicalLimit (always present — D3 may eliminate it) */
    {
        PgGroupExpr *limit = pg_memo_new_group_expr(ctx,
                                PG_CASCADES_LOGICAL_LIMIT);
        limit->inputs = list_make1(current);
        limit->op_private = upper;
        current = limit;
    }

    return current;
}
