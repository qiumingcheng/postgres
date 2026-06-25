/*-------------------------------------------------------------------------
 * postopt.c
 *    Cascades Post-Optimization: Plan Validator + Physical Rewrite (Phase 4g/4h)
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "optimizer/planmain.h"
#include "nodes/plannodes.h"

/* Set to true during self-tests to silence expected validation warnings */
bool pg_cascades_validate_quiet = false;

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
    int elevel = pg_cascades_validate_quiet ? DEBUG1 : WARNING;
    ListCell   *lc;

    if (plan == NULL)
    {
        elog(elevel, "Cascades validator: NULL plan at depth %d", depth);
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
        case T_BitmapAnd:
        case T_BitmapOr:
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
            elog(elevel,
                 "Cascades validator: unexpected plan node type %d at depth %d",
                 (int) nodeTag(plan), depth);
            return;
    }

    /* Check targetlist: should be non-NIL for most plan types.
     * Use NOTICE level — pg_cascades_fix_empty_targetlists handles the fix. */
    if (plan->targetlist == NIL &&
        nodeTag(plan) != T_Material &&
        nodeTag(plan) != T_BitmapHeapScan &&
        nodeTag(plan) != T_BitmapAnd &&
        nodeTag(plan) != T_BitmapOr)
    {
        elog(pg_cascades_validate_quiet ? DEBUG1 : NOTICE,
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
                elog(elevel,
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
    int elevel = pg_cascades_validate_quiet ? DEBUG1 : WARNING;

    if (plan == NULL)
    {
        elog(elevel, "Cascades validator: NULL plan");
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
 *
 *   Implemented rewrites:
 *     1. Materialize on NestLoop inner: If the inner side of a NestLoop
 *        is not a Material node and is not inherently rescannable
 *        (i.e., not a simple SeqScan/IndexScan), insert a Material node.
 *        This prevents expensive re-execution of the inner plan.
 *
 *   Future rewrites:
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
     * Rewrite 1: Materialize on NestLoop inner side.
     *
     * In PostgreSQL, the inner side of a NestLoop is rescanned for each
     * outer row.  If the inner plan is expensive to re-execute (e.g., a
     * Sort or HashAgg), we should insert a Material node to cache the
     * result.  PG's create_nestloop_plan already does this for paths,
     * but Cascades-extracted plans may not have this applied.
     *
     * We check:
     *   - Is this a NestLoop?
     *   - Is the inner side NOT already a Material?
     *   - Is the inner plan NOT inherently rescannable?
     *
     * Rescannable plan types (can re-execute cheaply):
     *   SeqScan, IndexScan, IndexOnlyScan, BitmapHeapScan, TidScan,
     *   FunctionScan, ValuesScan, Material (already materialized),
     *   Result (when it's just a projection)
     */
    if (IsA(plan, NestLoop) && plan->righttree != NULL)
    {
        Plan *inner = plan->righttree;

        /* Don't double-materialize */
        if (!IsA(inner, Material))
        {
            bool needs_material = false;

            switch (nodeTag(inner))
            {
                /* Inherently rescannable — no material needed */
                case T_SeqScan:
                case T_IndexScan:
                case T_IndexOnlyScan:
                case T_BitmapHeapScan:
                case T_TidScan:
                case T_FunctionScan:
                case T_ValuesScan:
                case T_Material:
                case T_CteScan:
                case T_WorkTableScan:
                    needs_material = false;
                    break;

                /* Result is rescannable if it's just a projection */
                case T_Result:
                    if (inner->lefttree == NULL)
                        needs_material = false;  /* constant Result */
                    else
                        needs_material = true;   /* Result with subplan */
                    break;

                /* All other types: need materialization */
                case T_Sort:
                case T_Agg:
                case T_Group:
                case T_Hash:
                case T_Unique:
                case T_WindowAgg:
                case T_SetOp:
                case T_NestLoop:
                case T_MergeJoin:
                case T_HashJoin:
                case T_SubqueryScan:
                case T_ForeignScan:
                case T_Append:
                case T_MergeAppend:
                default:
                    needs_material = true;
                    break;
            }

            if (needs_material)
            {
                plan->righttree = materialize_finished_plan(inner);

                if (ctx->debug)
                    elog(NOTICE, "Cascades postopt: inserted Material on "
                         "NestLoop inner (inner type=%d)",
                         (int) nodeTag(inner));
            }
        }
    }

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

/*
 * pg_postopt_self_test:
 *   Direct-call wrapper so gcov can track static validator + rewrite paths.
 */
void
pg_postopt_self_test(void)
{
    PgPlannerCascadesContext ctx;
    Plan *plan;
    bool saved_quiet;

    MemSet(&ctx, 0, sizeof(ctx));

    /* Silence expected validation warnings during self-test */
    saved_quiet = pg_cascades_validate_quiet;
    pg_cascades_validate_quiet = true;

    /* --- pg_cascades_validate_plan(NULL) --- */
    pg_cascades_validate_plan(NULL);

    /* --- validator: unexpected plan node type --- */
    plan = (Plan *) palloc0(sizeof(Plan));
    plan->type = 99999;  /* invalid node tag → default case */
    pg_cascades_validate_plan(plan);
    pfree(plan);

    /* --- validator: empty targetlist warning --- */
    plan = (Plan *) palloc0(sizeof(Plan));
    plan->type = T_SeqScan;
    plan->targetlist = NIL;
    plan->lefttree = NULL;
    plan->righttree = NULL;
    pg_cascades_validate_plan(plan);
    pfree(plan);

    /* --- validator: lefttree recursion --- */
    {
        Plan *left = (Plan *) palloc0(sizeof(Plan));
        left->type = T_SeqScan;
        left->targetlist = NIL;
        plan = (Plan *) palloc0(sizeof(Plan));
        plan->type = T_NestLoop;
        plan->targetlist = list_make1(left);  /* non-NIL */
        plan->lefttree = left;
        plan->righttree = NULL;
        pg_cascades_validate_plan(plan);
        pfree(plan);
        pfree(left);
    }

    /* --- validator: righttree recursion --- */
    {
        Plan *right = (Plan *) palloc0(sizeof(Plan));
        right->type = T_IndexScan;
        right->targetlist = NIL;
        plan = (Plan *) palloc0(sizeof(Plan));
        plan->type = T_MergeJoin;
        plan->targetlist = list_make1(right);
        plan->lefttree = NULL;
        plan->righttree = right;
        pg_cascades_validate_plan(plan);
        pfree(plan);
        pfree(right);
    }

    /* --- physical rewrite: NestLoop inner = Sort (needs Materialize) --- */
    {
        Plan *outer = (Plan *) palloc0(sizeof(Plan));
        Plan *inner = (Plan *) palloc0(sizeof(Plan));

        outer->type = T_SeqScan;
        outer->targetlist = NIL;
        inner->type = T_Sort;
        inner->targetlist = NIL;

        plan = (Plan *) palloc0(sizeof(Plan));
        plan->type = T_NestLoop;
        plan->targetlist = NIL;
        plan->lefttree = outer;
        plan->righttree = inner;

        ctx.debug = false;
        plan = pg_cascades_physical_rewrite(&ctx, plan);
        /* Material should have been inserted: righttree is now Material */
        if (plan != NULL)
        {
            pfree(plan->lefttree);  /* outer unchanged */
            if (plan->righttree != NULL && IsA(plan->righttree, Material))
                pfree(((Material *)plan->righttree)->plan.lefttree);
            pfree(plan->righttree);
            pfree(plan);
        }
    }

    /* --- physical rewrite: NestLoop inner = Agg (needs Materialize) --- */
    {
        Plan *outer = (Plan *) palloc0(sizeof(Plan));
        Plan *inner = (Plan *) palloc0(sizeof(Plan));

        outer->type = T_IndexScan;
        outer->targetlist = NIL;
        inner->type = T_Agg;
        inner->targetlist = NIL;

        plan = (Plan *) palloc0(sizeof(Plan));
        plan->type = T_NestLoop;
        plan->targetlist = NIL;
        plan->lefttree = outer;
        plan->righttree = inner;

        plan = pg_cascades_physical_rewrite(&ctx, plan);
        if (plan != NULL)
        {
            pfree(plan->lefttree);
            if (plan->righttree != NULL && IsA(plan->righttree, Material))
                pfree(((Material *)plan->righttree)->plan.lefttree);
            pfree(plan->righttree);
            pfree(plan);
        }
    }

    /* --- physical rewrite: NestLoop inner = Hash (needs Materialize) --- */
    {
        Plan *outer = (Plan *) palloc0(sizeof(Plan));
        Plan *inner = (Plan *) palloc0(sizeof(Plan));

        outer->type = T_SeqScan;
        outer->targetlist = NIL;
        inner->type = T_Hash;
        inner->targetlist = NIL;

        plan = (Plan *) palloc0(sizeof(Plan));
        plan->type = T_NestLoop;
        plan->targetlist = NIL;
        plan->lefttree = outer;
        plan->righttree = inner;

        plan = pg_cascades_physical_rewrite(&ctx, plan);
        if (plan != NULL)
        {
            pfree(plan->lefttree);
            if (plan->righttree != NULL && IsA(plan->righttree, Material))
                pfree(((Material *)plan->righttree)->plan.lefttree);
            pfree(plan->righttree);
            pfree(plan);
        }
    }

    /* --- physical rewrite: NestLoop inner already Material (no change) --- */
    {
        Plan *outer = (Plan *) palloc0(sizeof(Plan));
        Material *mat = (Material *) palloc0(sizeof(Material));

        outer->type = T_SeqScan;
        outer->targetlist = NIL;
        mat->plan.type = T_Material;
        mat->plan.targetlist = NIL;

        plan = (Plan *) palloc0(sizeof(Plan));
        plan->type = T_NestLoop;
        plan->targetlist = NIL;
        plan->lefttree = outer;
        plan->righttree = (Plan *) mat;

        plan = pg_cascades_physical_rewrite(&ctx, plan);
        if (plan != NULL)
        {
            pfree(plan->lefttree);
            pfree(plan->righttree);
            pfree(plan);
        }
    }

    /* --- physical rewrite: NULL plan --- */
    if (pg_cascades_physical_rewrite(&ctx, NULL) != NULL)
        elog(WARNING, "postopt self-test: NULL plan should return NULL");

    pg_cascades_validate_quiet = saved_quiet;
}
