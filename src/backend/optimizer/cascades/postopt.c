/*-------------------------------------------------------------------------
 * postopt.c
 *    Cascades Post-Optimization: Plan Validator + Physical Rewrite (Phase 4g/4h)
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "nodes/plannodes.h"

/* ========================================================================
 * Phase 4h: Plan Validator
 * ======================================================================== */

/*
 * pg_cascades_validate_plan_recurse:
 *   Recursively validate a Plan tree structure.
 *   Checks: valid type tag, valid targetlist, valid child pointers.
 *   Emits WARNING on issues — best-effort debug aid, never ERRORs.
 */
static void
pg_cascades_validate_plan_recurse(Plan *plan, int depth)
{
    ListCell   *lc;

    if (plan == NULL)
    {
        elog(WARNING, "Cascades validator: NULL plan at depth %d", depth);
        return;
    }

    /* Check node tag is a known plan type */
    switch (nodeTag(plan))
    {
        case T_SeqScan:
        case T_IndexScan:
        case T_IndexOnlyScan:
        case T_BitmapHeapScan:
        case T_BitmapIndexScan:
        case T_TidScan:
        case T_SubqueryScan:
        case T_FunctionScan:
        case T_ValuesScan:
        case T_CteScan:
        case T_WorkTableScan:
        case T_ForeignScan:
        case T_NestLoop:
        case T_MergeJoin:
        case T_HashJoin:
        case T_Hash:
        case T_Agg:
        case T_Group:
        case T_WindowAgg:
        case T_Unique:
        case T_Sort:
        case T_Limit:
        case T_Result:
        case T_Append:
        case T_MergeAppend:
        case T_Material:
        case T_SetOp:
            break;
        default:
            elog(WARNING,
                 "Cascades validator: unexpected plan node type %d at depth %d",
                 (int) nodeTag(plan), depth);
            return;
    }

    /* Check targetlist: should be non-NIL for most plan types */
    if (plan->targetlist == NIL &&
        nodeTag(plan) != T_Material)
    {
        elog(WARNING,
             "Cascades validator: empty targetlist for node type %d at depth %d",
             (int) nodeTag(plan), depth);
    }

    /* Validate left child recursively */
    if (plan->lefttree != NULL)
        pg_cascades_validate_plan_recurse(plan->lefttree, depth + 1);

    /* Validate right child recursively */
    if (plan->righttree != NULL)
        pg_cascades_validate_plan_recurse(plan->righttree, depth + 1);

    /* Validate qual list entries are non-NULL */
    if (plan->qual != NIL)
    {
        foreach(lc, plan->qual)
        {
            if (lfirst(lc) == NULL)
                elog(WARNING,
                     "Cascades validator: NULL qual at depth %d", depth);
        }
    }
}

/*
 * pg_cascades_validate_plan:
 *   Top-level plan validation entry point.
 *   Walks the entire plan tree checking structural validity.
 */
void
pg_cascades_validate_plan(Plan *plan)
{
    if (plan == NULL)
    {
        elog(WARNING, "Cascades validator: NULL plan");
        return;
    }
    pg_cascades_validate_plan_recurse(plan, 0);
}

/* ========================================================================
 * Phase 4g: Post-Optimization Physical Rewrite
 * ======================================================================== */

/*
 * pg_cascades_physical_rewrite_recurse:
 *   Recursively apply post-optimization physical rewrites (bottom-up).
 *   Currently a skeleton — recurses into children, applies no rewrites.
 *
 *   Future rewrites (when PG helpers are exposed):
 *     - Materialize on NestLoop inner for non-rescannable scans
 *     - Pre-aggregate pushdown (partial agg)
 *     - Skew join detection and adjustment
 */
static Plan *
pg_cascades_physical_rewrite_recurse(PgPlannerCascadesContext *ctx, Plan *plan)
{
    if (plan == NULL)
        return NULL;

    /* Recurse into children first (bottom-up) */
    plan->lefttree = pg_cascades_physical_rewrite_recurse(ctx,
                                                           plan->lefttree);
    plan->righttree = pg_cascades_physical_rewrite_recurse(ctx,
                                                            plan->righttree);

    /*
     * Future rewrite rules go here.
     * The required PG helpers (make_material etc.) are static in
     * createplan.c and need to be exposed before these can be enabled.
     */

    return plan;
}

/*
 * pg_cascades_physical_rewrite:
 *   Top-level entry for post-optimization physical plan rewrite.
 */
Plan *
pg_cascades_physical_rewrite(PgPlannerCascadesContext *ctx, Plan *plan)
{
    if (plan == NULL)
        return NULL;
    return pg_cascades_physical_rewrite_recurse(ctx, plan);
}
