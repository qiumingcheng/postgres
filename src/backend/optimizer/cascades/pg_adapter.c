/*-------------------------------------------------------------------------
 * pg_adapter.c
 *    PG 适配层：在 Cascades 世界和 PG 世界之间转换。
 *    - build logical root from PG structures
 *    - Path type to Cascades op kind mapping
 *    - Phase 7: build LogicalJoin tree from PG joinlist
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "optimizer/pathnode.h"
#include "nodes/primnodes.h"
#include "optimizer/paths.h"

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
 * Helper: collect Relids from a standalone OptExpression subtree
 *
 * Walks a tree of PgGroupExpr * nodes (before Memo insertion, where
 * inputs are PgGroupExpr *, not PgMemoGroup *) and collects the union
 * of RelOptInfo->relids from all LogicalScan leaves.
 * ======================================================================== */
static Relids
pg_get_tree_relids(PgGroupExpr *expr)
{
    Relids result = NULL;
    ListCell *lc;

    if (expr == NULL)
        return NULL;

    if (expr->op == PG_CASCADES_LOGICAL_SCAN && expr->op_private != NULL)
    {
        RelOptInfo *rel = (RelOptInfo *) expr->op_private;
        if (rel->relids != NULL)
            return bms_copy(rel->relids);
        else
            return NULL;
    }

    foreach(lc, expr->inputs)
    {
        PgGroupExpr *child = (PgGroupExpr *) lfirst(lc);
        Relids child_relids;

        if (child == NULL)
            continue;

        child_relids = pg_get_tree_relids(child);
        if (child_relids != NULL)
        {
            result = bms_union(result, child_relids);
            bms_free(child_relids);
        }
    }

    return result;
}

/* ========================================================================
 * Helper: determine actual join type from PG's SpecialJoinInfo list
 *
 * Matches the left and right child relids against the join_info_list
 * entries populated by deconstruct_jointree().  Returns JOIN_INNER if
 * no SpecialJoinInfo covers this exact pair (inner join is the default).
 * ======================================================================== */
static JoinType
pg_determine_join_type(PlannerInfo *root, Relids left_relids, Relids right_relids)
{
    ListCell *lc;

    if (left_relids == NULL || right_relids == NULL)
        return JOIN_INNER;

    foreach(lc, root->join_info_list)
    {
        SpecialJoinInfo *sj = (SpecialJoinInfo *) lfirst(lc);

        if (sj->syn_lefthand == NULL || sj->syn_righthand == NULL)
            continue;

        /* Check if this SpecialJoinInfo exactly covers our children */
        if (bms_equal(sj->syn_lefthand, left_relids) &&
            bms_equal(sj->syn_righthand, right_relids))
            return sj->jointype;

        /* Also check swapped (RIGHT JOIN normalized to LEFT) */
        if (bms_equal(sj->syn_lefthand, right_relids) &&
            bms_equal(sj->syn_righthand, left_relids))
            return sj->jointype;
    }

    return JOIN_INNER;
}

/* ========================================================================
 * Helper: build LogicalJoin tree from PG's joinlist (Phase 7)
 * ======================================================================== */

static PgGroupExpr *
pg_cascades_build_join_tree(PgPlannerCascadesContext *ctx, List *joinlist)
{
    ListCell   *lc;
    PgGroupExpr *result = NULL;

    if (joinlist == NIL)
        return NULL;

    /* Single element: could be RangeTblRef or sub-joinlist */
    if (list_length(joinlist) == 1)
    {
        Node *jlnode = (Node *) linitial(joinlist);

        if (IsA(jlnode, RangeTblRef))
        {
            /* Leaf: base relation */
            RangeTblRef *rtr = (RangeTblRef *) jlnode;
            RelOptInfo  *rel = ctx->root->simple_rel_array[rtr->rtindex];

            if (rel == NULL)
                return NULL;

            result = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_SCAN);
            result->inputs = NIL;
            result->op_private = rel;
            return result;
        }
        else if (IsA(jlnode, List))
        {
            /* Sub-joinlist: recurse */
            return pg_cascades_build_join_tree(ctx, (List *) jlnode);
        }
        else
        {
            elog(ERROR, "unrecognized joinlist node type: %d",
                 (int) nodeTag(jlnode));
            return NULL;
        }
    }

    /* Multiple elements: build left-deep join tree */
    foreach(lc, joinlist)
    {
        Node       *jlnode = (Node *) lfirst(lc);
        PgGroupExpr *child;

        if (IsA(jlnode, RangeTblRef))
        {
            RangeTblRef *rtr = (RangeTblRef *) jlnode;
            RelOptInfo  *rel = ctx->root->simple_rel_array[rtr->rtindex];

            if (rel == NULL)
                continue;

            child = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_SCAN);
            child->inputs = NIL;
            child->op_private = rel;
        }
        else if (IsA(jlnode, List))
        {
            child = pg_cascades_build_join_tree(ctx, (List *) jlnode);
        }
        else
        {
            continue;
        }

        if (child == NULL)
            continue;

        if (result == NULL)
        {
            result = child;
        }
        else
        {
            /* Create a LogicalJoin(previous_result, new_child) */
            PgGroupExpr *join;
            PgJoinPrivate *jp;
            Relids left_relids;
            Relids right_relids;

            /* Determine actual join type from PG's SpecialJoinInfo */
            left_relids = pg_get_tree_relids(result);
            right_relids = pg_get_tree_relids(child);

            jp = (PgJoinPrivate *) palloc0(sizeof(PgJoinPrivate));
            jp->jointype = pg_determine_join_type(ctx->root,
                                                   left_relids, right_relids);
            jp->restrictlist = NIL;  /* quals handled by PG Path internally */
            jp->joinlist = NIL;

            join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
            join->inputs = list_make2(result, child);
            join->op_private = jp;
            result = join;

            bms_free(left_relids);
            bms_free(right_relids);
        }
    }

    return result;
}

/* ========================================================================
 * pg_cascades_build_initial_tree:
 *    Phase 7: Build OptExpression tree with individual base relation
 *    LogicalScan nodes and LogicalJoin tree from the PG joinlist.
 *
 *    Structure:  LogicalLimit → LogicalSort → LogicalDistinct →
 *                LogicalAgg → LogicalProject → LogicalJoin(s) →
 *                LogicalScan(s) [one per base relation]
 *
 *    Each LogicalScan has op_private = RelOptInfo * for its base relation.
 *    Each LogicalJoin has op_private = PgJoinPrivate * with jointype
 *    determined from PG's SpecialJoinInfo (JOIN_INNER/LEFT/RIGHT/SEMI/ANTI).
 *    restrictlist remains NIL — quals are handled by PG Path internally
 *    via IMPORTED_PATH scan/join physical expressions.
 */
PgGroupExpr *
pg_cascades_build_initial_tree(PgPlannerCascadesContext *ctx)
{
    PgCascadesUpperInfo *upper = ctx->upper;
    PgGroupExpr *current = NULL;

    /* Phase 7: Build join tree from joinlist */
    current = pg_cascades_build_join_tree(ctx, ctx->prep->joinlist);

    if (current == NULL)
    {
        current = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_SCAN);
        current->inputs = NIL;
        current->op_private = ctx->prep->final_rel;
    }

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
