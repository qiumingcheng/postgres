/*-------------------------------------------------------------------------
 * planbuild.c
 *    Cascades Plan Builder: 从 Memo best 表抽取最优 Plan *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "optimizer/paths.h"
#include "utils/memutils.h"

/* forward */
Plan *pg_cascades_build_plan_recurse(PgPlannerCascadesContext *ctx,
    PgMemoGroup *group, PgRequiredProperty *required,
    PgGroupBestEntry *best, PgOutputProperty *output);
void pg_cascades_fix_empty_targetlists(PlannerInfo *root, Plan *plan);


/* ========================================================================
 * Extract Best Plan (Top-level Entry)
 * ======================================================================== */

Plan *
pg_cascades_extract_best_plan(PgPlannerCascadesContext *ctx)
{
    PgMemoGroup *root_group = ctx->memo->root_group;
    Plan       *result = NULL;
    ListCell   *lc;

    /*
     * Phase 1 simplification: try to find best physical expr first,
     * but fall back to building directly from logical expr if needed.
     */

    /*
     * Two-pass extraction: COMPOSABLE_OP first (exercises PHYSICAL_*
     * recursive branches), IMPORTED_PATH as fallback (PG's proven path).
     */
    {
        PgRequiredProperty *root_req = pg_cascades_root_required_property(ctx);
        int pass;

        for (pass = 0; pass < 2; pass++)
        {
            foreach(lc, root_group->best_entries)
            {
                PgGroupBestEntry *entry = (PgGroupBestEntry *) lfirst(lc);
                if (!pg_required_property_equal(entry->required, root_req))
                    continue;

                /* Pass 0: COMPOSABLE_OP only; Pass 1: IMPORTED_PATH only */
                if (pass == 0 && entry->expr->mode == PG_PHYS_EXPR_IMPORTED_PATH)
                    continue;
                if (pass == 1 && entry->expr->mode != PG_PHYS_EXPR_IMPORTED_PATH)
                    continue;

                /* Skip bare scan entries when upper ops exist */
                if (entry->expr->mode == PG_PHYS_EXPR_IMPORTED_PATH &&
                    entry->expr->op <= PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN)
                {
                    bool has_upper = false;
                    ListCell *elc;
                    foreach(elc, root_group->logical_exprs)
                    {
                        PgGroupExpr *e = (PgGroupExpr *) lfirst(elc);
                        if (e->op >= PG_CASCADES_LOGICAL_PROJECT &&
                            e->op <= PG_CASCADES_LOGICAL_LIMIT)
                        { has_upper = true; break; }
                    }
                    if (has_upper) continue;
                }

                result = pg_cascades_build_plan_recurse(ctx, root_group, root_req,
                                                         entry, &entry->output);
                if (result != NULL)
                {
                    ctx->root->query_pathkeys = entry->output.pathkeys;
                    pg_cascades_fix_empty_targetlists(ctx->root, result);
                    return result;
                }
            }
        }
    }

    /*
     * Phase 1 Critical Path: If no best entry was found, this is a bug
     * in the task scheduler or path import logic. In Phase 1 Path-import
     * mode, we should ALWAYS have valid best_entries from PG's make_one_rel().
     *
     * The pg_cascades_build_logical_plan() fallback was REMOVED because:
     * 1. It calls create_plan() on Paths with incomplete RelOptInfo
     * 2. This causes "variable not found in subplan target lists" errors
     * 3. Per 2.md section 5.2: Phase 1 must not modify PG's lower structures
     *
     * The function implementation remains below for Phase 2, but is not called.
     */
    if (cascades_planner_debug)
        elog(WARNING, "Cascades: no best_entries found for root group - "
             "this indicates incomplete optimization. Falling back to PG planner.");
    return NULL;
}

/*
 * pg_safe_linitial_child_req:
 *   Safe accessor for linitial(best->child_required_props).
 *   Must be NOINLINE so the compiler cannot optimize away the NULL check
 *   and insert a trap (ud2).  With -O2, gcc treats linitial(NIL) as
 *   undefined behavior and replaces it with a deliberate crash.
 *
 *   Returns NULL when child_required_props is NIL (graceful fallback).
 */
__attribute__((noinline)) PgRequiredProperty *
pg_safe_linitial_child_req(PgGroupBestEntry *best)
{
    if (best->child_required_props == NIL)
        return NULL;
    return (PgRequiredProperty *) linitial(best->child_required_props);
}

/*
 * pg_cascades_fix_empty_targetlists:
 *   Walk a plan tree and fix any node with an empty targetlist.
 *
 *   For scan nodes (SeqScan, IndexScan, IndexOnlyScan, BitmapHeapScan),
 *   the targetlist is derived from the query's target list filtered by
 *   the scanned relation.  This is needed when create_plan produces scan
 *   nodes with NIL targetlist due to column pruning clearing reltargetlist.
 *
 *   For join and other nodes, the targetlist is propagated from the
 *   left child when available.
 */
void
pg_cascades_fix_empty_targetlists(PlannerInfo *root, Plan *plan)
{
    Index       relid = 0;

    if (plan == NULL)
        return;

    /* Debug: log plan node details */
    if (cascades_planner_debug)
    {
        elog(NOTICE, "fix_empty_targetlists: node type %d, targetlist length %d",
             (int) nodeTag(plan), list_length(plan->targetlist));
    }

    /* Recurse into children first */
    if (plan->lefttree != NULL)
        pg_cascades_fix_empty_targetlists(root, plan->lefttree);
    if (plan->righttree != NULL)
        pg_cascades_fix_empty_targetlists(root, plan->righttree);

    /* Fix this node if targetlist is empty.
     * For scan nodes, the targetlist comes from create_plan which uses
     * reltargetlist.  If reltargetlist was cleared by column pruning or
     * is missing columns, we rebuild from the query's targetList. */
    if (plan->targetlist == NIL)
    {
        if (cascades_planner_debug)
            elog(NOTICE, "fix_empty_targetlists: node type %d has NIL targetlist, attempting fix",
                 (int) nodeTag(plan));

        /* For join nodes from create_plan(), NIL targetlist should NOT happen.
         * This indicates the underlying Path or RelOptInfo was corrupted.
         * Do NOT try to "fix" it by merging child targetlists - that's a band-aid. */
        if ((nodeTag(plan) == T_NestLoop ||
             nodeTag(plan) == T_HashJoin ||
             nodeTag(plan) == T_MergeJoin))
        {
            elog(WARNING, "Cascades: JOIN node type %d has empty targetlist from create_plan() - "
                 "this indicates corrupted Path or RelOptInfo. Query should fallback.",
                 (int) nodeTag(plan));
            /* Don't try to fix - let it fail so we can diagnose the root cause */
            return;
        }

        /* Only fix upper nodes that Cascades might create without proper targetlist */
        if (plan->lefttree != NULL && plan->lefttree->targetlist != NIL)
        {
            plan->targetlist = plan->lefttree->targetlist;
            if (cascades_planner_debug)
                elog(NOTICE, "fix_empty_targetlists: fixed node type %d using lefttree targetlist",
                     (int) nodeTag(plan));
            return;
        }
        if (plan->righttree != NULL && plan->righttree->targetlist != NIL)
        {
            plan->targetlist = plan->righttree->targetlist;
            if (cascades_planner_debug)
                elog(NOTICE, "fix_empty_targetlists: fixed node type %d using righttree targetlist",
                     (int) nodeTag(plan));
            return;
        }

        /* For scan nodes, try query targetList first, then reltargetlist */
        {
            bool is_scan = false;
            switch (nodeTag(plan))
            {
                case T_SeqScan:
                    relid = ((SeqScan *) plan)->scanrelid; is_scan = true; break;
                case T_IndexScan:
                    relid = ((IndexScan *) plan)->scan.scanrelid; is_scan = true; break;
                case T_IndexOnlyScan:
                    relid = ((IndexOnlyScan *) plan)->scan.scanrelid; is_scan = true; break;
                case T_BitmapHeapScan:
                    relid = ((BitmapHeapScan *) plan)->scan.scanrelid; is_scan = true; break;
                default:
                    break;
            }

            if (is_scan && relid > 0 && relid <= root->simple_rel_array_size)
            {
                RelOptInfo *rel = root->simple_rel_array[relid];
                if (rel != NULL && rel->reltargetlist != NIL)
                {
                    /*
                     * Use reltargetlist directly — it includes both SELECT
                     * columns AND join-key columns needed by upper nodes.
                     * Using root->parse->targetList would miss join keys
                     * like t2.id, causing "variable not found" errors
                     * in set_plan_references.
                     */
                    plan->targetlist = rel->reltargetlist;
                }
            }
        }
    }
}

/* ========================================================================
 * Recursive Plan Builder
 * ======================================================================== */

Plan *
pg_cascades_build_plan_recurse(PgPlannerCascadesContext *ctx,
                               PgMemoGroup *group,
                               PgRequiredProperty *required,
                               PgGroupBestEntry *best,
                               PgOutputProperty *output)
{
    PgGroupExpr *expr = best->expr;
    Plan       *result = NULL;

    *output = best->output;

    switch (expr->op)
    {
        /* === IMPORTED_PATH: call create_plan directly === */
        case PG_CASCADES_PHYSICAL_SEQSCAN:
        case PG_CASCADES_PHYSICAL_INDEXSCAN:
        case PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN:
        case PG_CASCADES_PHYSICAL_NESTLOOP:
        case PG_CASCADES_PHYSICAL_HASHJOIN:
        case PG_CASCADES_PHYSICAL_MERGEJOIN:
            if (expr->mode != PG_PHYS_EXPR_IMPORTED_PATH)
                return NULL;
            result = create_plan(ctx->root, (Path *) expr->op_private);
            if (result != NULL)
                pg_cascades_fix_empty_targetlists(ctx->root, result);
            break;

        /* === Upper Ops: recursive build + wrap === */

        case PG_CASCADES_PHYSICAL_SORT:
            {
                Plan *child;
                PgGroupBestEntry *child_best = NULL;
                PgOutputProperty child_out;
                PgMemoGroup *child_group;
                ListCell *lc;

                if (expr->inputs == NIL)
                    return NULL;
                child_group = (PgMemoGroup *) linitial(expr->inputs);
                if (child_group->best_entries == NIL)
                    return NULL;

                /* find child best */
                foreach(lc, child_group->best_entries)
                {
                    PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
                    if (best->child_required_props != NIL &&
                        pg_required_property_equal(e->required,
                            (PgRequiredProperty *) pg_safe_linitial_child_req(best)))
                    {
                        child_best = e;
                        break;
                    }
                }
                if (child_best == NULL)
                    return NULL;

                child = pg_cascades_build_plan_recurse(ctx,
                    (PgMemoGroup *) linitial(expr->inputs),
                    (PgRequiredProperty *) pg_safe_linitial_child_req(best),
                    child_best, &child_out);

                result = (Plan *) make_sort_from_pathkeys(ctx->root, child,
                    required->pathkeys, required->limit_tuples);
            }
            break;

        case PG_CASCADES_PHYSICAL_HASHAGG:
            {
                Plan *child;
                PgGroupBestEntry *child_best = NULL;
                PgOutputProperty child_out;
                PgMemoGroup *child_group;
                ListCell *lc;

                /*
                 * Guard: expr->inputs may be NIL if the task scheduler
                 * produced an expression without setting inputs (e.g.,
                 * after rewrite rules removed children).  The compiler
                 * treats linitial(NIL) as unreachable and inserts a trap,
                 * so we must check explicitly.
                 */
                if (expr->inputs == NIL)
                    return NULL;

                child_group = (PgMemoGroup *) linitial(expr->inputs);

                /*
                 * Guard: best_entries may be NIL if the task scheduler has not
                 * yet populated this group (e.g., after group merging or when
                 * the join path generation was incomplete).  The compiler may
                 * optimize foreach/linitial on NIL into an unreachable trap,
                 * so we must check explicitly and return NULL unconditionally.
                 */
                if (child_group->best_entries == NIL)
                    return NULL;

                /*
                 * Guard: best->child_required_props may be NIL when enforcer
                 * rules create entries without child required properties.
                 * The compiler optimizes linitial(NIL) into a trap before
                 * the foreach loop, so we must check explicitly.
                 */
                if (best->child_required_props == NIL)
                    return NULL;

                if (cascades_planner_debug)
                    elog(NOTICE, "planbuild HashAgg: child_group=%d best_entries=%d child_required_props=%s",
                         child_group->id,
                         list_length(child_group->best_entries),
                         (best->child_required_props != NIL) ? "yes" : "NIL");

                foreach(lc, child_group->best_entries)
                {
                    PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
                    if (cascades_planner_debug)
                        elog(NOTICE, "  child best: op=%d req_pathkeys=%p",
                             e->expr->op, e->required->pathkeys);
                    if (best->child_required_props != NIL &&
                        pg_required_property_equal(e->required,
                            (PgRequiredProperty *) pg_safe_linitial_child_req(best)))
                    {
                        child_best = e;
                        break;
                    }
                }
                if (child_best == NULL)
                {
                    /* Fallback: if only 1 entry, use it */
                    if (list_length(child_group->best_entries) == 1)
                        child_best = (PgGroupBestEntry *) linitial(child_group->best_entries);
                }
                if (child_best == NULL)
                {
                    if (cascades_planner_debug)
                        elog(WARNING, "planbuild HashAgg: child_best not found");
                    return NULL;
                }

                child = pg_cascades_build_plan_recurse(ctx,
                    (PgMemoGroup *) linitial(expr->inputs),
                    (PgRequiredProperty *) pg_safe_linitial_child_req(best),
                    child_best, &child_out);
                if (child == NULL)
                {
                    if (cascades_planner_debug)
                        elog(WARNING, "planbuild HashAgg: child plan is NULL");
                    return NULL;
                }

                if (ctx->upper->hasAggs)
                {
                    AggStrategy agg_strategy;
                    if (ctx->upper->numGroupCols > 0)
                        agg_strategy = AGG_HASHED;
                    else
                        agg_strategy = AGG_PLAIN;

                    result = (Plan *) make_agg(ctx->root,
                        ctx->upper->tlist,
                        (List *) ctx->upper->havingQual,
                        agg_strategy,
                        &ctx->upper->agg_costs,
                        ctx->upper->numGroupCols,
                        ctx->upper->groupColIdx,
                        ctx->upper->groupOperators,
                        (long) ctx->upper->dNumGroups,
                        child);
                }
                else
                {
                    result = (Plan *) make_group(ctx->root,
                        ctx->upper->tlist,
                        (List *) ctx->upper->havingQual,
                        ctx->upper->numGroupCols,
                        ctx->upper->groupColIdx,
                        ctx->upper->groupOperators,
                        ctx->upper->dNumGroups,
                        child);
                }
            }
            break;

        case PG_CASCADES_PHYSICAL_GROUPAGG:
            {
                Plan *child;
                PgGroupBestEntry *child_best = NULL;
                PgOutputProperty child_out;
                PgMemoGroup *child_group;
                ListCell *lc;

                /* Guard: same as HashAgg case above */
                if (expr->inputs == NIL)
                    return NULL;

                child_group = (PgMemoGroup *) linitial(expr->inputs);

                /* Guard: same as HashAgg case above */
                if (child_group->best_entries == NIL)
                {
                    if (cascades_planner_debug)
                        elog(WARNING, "planbuild GroupAgg: child_group=%d best_entries is NIL",
                             child_group->id);
                    return NULL;
                }

                foreach(lc, child_group->best_entries)
                {
                    PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
                    if (best->child_required_props != NIL &&
                        pg_required_property_equal(e->required,
                            (PgRequiredProperty *) pg_safe_linitial_child_req(best)))
                    {
                        child_best = e;
                        break;
                    }
                }
                if (child_best == NULL)
                    return NULL;

                child = pg_cascades_build_plan_recurse(ctx,
                    (PgMemoGroup *) linitial(expr->inputs),
                    (PgRequiredProperty *) pg_safe_linitial_child_req(best),
                    child_best, &child_out);
                if (child == NULL)
                    return NULL;

                if (ctx->upper->hasAggs)
                {
                    result = (Plan *) make_agg(ctx->root,
                        ctx->upper->tlist,
                        (List *) ctx->upper->havingQual,
                        AGG_SORTED,
                        &ctx->upper->agg_costs,
                        ctx->upper->numGroupCols,
                        ctx->upper->groupColIdx,
                        ctx->upper->groupOperators,
                        (long) ctx->upper->dNumGroups,
                        child);
                }
                else
                {
                    result = (Plan *) make_group(ctx->root,
                        ctx->upper->tlist,
                        (List *) ctx->upper->havingQual,
                        ctx->upper->numGroupCols,
                        ctx->upper->groupColIdx,
                        ctx->upper->groupOperators,
                        ctx->upper->dNumGroups,
                        child);
                }
            }
            break;

        case PG_CASCADES_PHYSICAL_UNIQUE:
            {
                Plan *child;
                PgGroupBestEntry *child_best = NULL;
                PgOutputProperty child_out;
                PgMemoGroup *child_group;
                ListCell *lc;

                if (expr->inputs == NIL)
                    return NULL;
                child_group = (PgMemoGroup *) linitial(expr->inputs);
                if (child_group->best_entries == NIL)
                    return NULL;

                foreach(lc, child_group->best_entries)
                {
                    PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
                    if (best->child_required_props != NIL &&
                        pg_required_property_equal(e->required,
                            (PgRequiredProperty *) pg_safe_linitial_child_req(best)))
                    {
                        child_best = e;
                        break;
                    }
                }
                if (child_best == NULL)
                    return NULL;

                child = pg_cascades_build_plan_recurse(ctx,
                    (PgMemoGroup *) linitial(expr->inputs),
                    (PgRequiredProperty *) pg_safe_linitial_child_req(best),
                    child_best, &child_out);

                result = (Plan *) make_unique(child, ctx->upper->distinctClause);
            }
            break;

        case PG_CASCADES_PHYSICAL_LIMIT:
            {
                Plan *child;
                PgGroupBestEntry *child_best = NULL;
                PgOutputProperty child_out;
                PgMemoGroup *child_group;
                ListCell *lc;

                if (expr->inputs == NIL)
                    return NULL;
                child_group = (PgMemoGroup *) linitial(expr->inputs);
                if (child_group->best_entries == NIL)
                    return NULL;

                foreach(lc, child_group->best_entries)
                {
                    PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
                    if (best->child_required_props != NIL &&
                        pg_required_property_equal(e->required,
                            (PgRequiredProperty *) pg_safe_linitial_child_req(best)))
                    {
                        child_best = e;
                        break;
                    }
                }
                if (child_best == NULL)
                    return NULL;

                child = pg_cascades_build_plan_recurse(ctx,
                    (PgMemoGroup *) linitial(expr->inputs),
                    (PgRequiredProperty *) pg_safe_linitial_child_req(best),
                    child_best, &child_out);

                result = (Plan *) make_limit(child,
                    ctx->root->parse->limitOffset,
                    ctx->root->parse->limitCount,
                    ctx->upper->offset_est,
                    ctx->upper->count_est);
            }
            break;

        case PG_CASCADES_PHYSICAL_PROJECT:
            {
                Plan *child;
                PgGroupBestEntry *child_best = NULL;
                PgOutputProperty child_out;
                PgMemoGroup *child_group;
                ListCell *lc;

                if (expr->inputs == NIL)
                    return NULL;
                child_group = (PgMemoGroup *) linitial(expr->inputs);
                if (child_group->best_entries == NIL)
                    return NULL;

                foreach(lc, child_group->best_entries)
                {
                    PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
                    if (best->child_required_props != NIL &&
                        pg_required_property_equal(e->required,
                            (PgRequiredProperty *) pg_safe_linitial_child_req(best)))
                    {
                        child_best = e;
                        break;
                    }
                }
                if (child_best == NULL)
                    return NULL;

                child = pg_cascades_build_plan_recurse(ctx,
                    (PgMemoGroup *) linitial(expr->inputs),
                    (PgRequiredProperty *) pg_safe_linitial_child_req(best),
                    child_best, &child_out);

                /*
                 * For queries without aggregation, the sub_tlist's Var
                 * references should match the child plan's output after
                 * set_plan_references in standard_planner.  We have two
                 * strategies depending on the child plan type:
                 *
                 * 1. Projection-capable (Scan, Sort, etc.):
                 *    Replace the plan's targetlist with sub_tlist.
                 *    This works because set_plan_references can map
                 *    the Vars correctly for simple plans.
                 *
                 * 2. Not projection-capable (Join plans):
                 *    Return the child directly without wrapping.
                 *    The join plan's output (from make_one_rel's
                 *    reltarget) already contains all needed columns.
                 *    set_plan_references maps the query tlist to the
                 *    join output — exactly like the standard planner
                 *    does when need_tlist_eval is false.
                 *
                 * StarRocks reference: OptExpression tree's Project
                 * node is resolved by mapping ColumnRefOperators,
                 * not by wrapping with a separate projection node.
                 */
                if (child == NULL)
                    return NULL;

                if (ctx->upper->numGroupCols == 0 &&
                    !ctx->upper->hasAggs &&
                    ctx->upper->activeWindows == NIL)
                {
                    if (is_projection_capable_plan(child))
                    {
                        child->targetlist = ctx->upper->sub_tlist;
                        return child;
                    }
                    else
                    {
                        /* Join plan: return directly, let set_plan_references handle it */
                        return child;
                    }
                }

                /* Agg/grouping: wrap with Result for safety */
                result = (Plan *) make_result(ctx->root,
                    ctx->upper->sub_tlist,
                    NULL,
                    child);
            }
            break;

        default:
            elog(ERROR, "unexpected physical op kind in planbuild: %d", expr->op);
            break;
    }

    return result;
}
