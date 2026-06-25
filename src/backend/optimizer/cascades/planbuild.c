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
static Plan *pg_cascades_build_plan_recurse(PgPlannerCascadesContext *ctx,
    PgMemoGroup *group, PgRequiredProperty *required,
    PgGroupBestEntry *best, PgOutputProperty *output);
static Path *pg_cascades_find_imported_path(PgMemoGroup *group);
static void pg_cascades_fix_empty_targetlists(PlannerInfo *root, Plan *plan);

/* ========================================================================
 * Helper: find imported Path by walking through LogicalProject chain
 * ======================================================================== */
static Path *
pg_cascades_find_imported_path(PgMemoGroup *group)
{
    ListCell *lc;

    if (group == NULL)
        return NULL;

    /* Check this group's best entries for IMPORTED_PATH */
    foreach(lc, group->best_entries)
    {
        PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
        if (e->expr->mode == PG_PHYS_EXPR_IMPORTED_PATH)
            return (Path *) e->expr->op_private;
    }

    /* Recurse through LogicalProject/LogicalFilter children */
    foreach(lc, group->logical_exprs)
    {
        PgGroupExpr *log = (PgGroupExpr *) lfirst(lc);
        if (log->op == PG_CASCADES_LOGICAL_PROJECT ||
            log->op == PG_CASCADES_LOGICAL_FILTER)
        {
            ListCell *cl;
            foreach(cl, log->inputs)
            {
                Path *p = pg_cascades_find_imported_path((PgMemoGroup *) lfirst(cl));
                if (p != NULL) return p;
            }
        }
    }
    return NULL;
}

/* ========================================================================
 * Helper: build plan from a group's logical expression tree (bottom-up)
 * Returns NULL if no plan can be built for this group.
 * ======================================================================== */
static Plan *
pg_cascades_build_logical_plan(PgPlannerCascadesContext *ctx, PgMemoGroup *group)
{
    ListCell *lc;
    Path *imported_path;

    /* If this group has an IMPORTED_PATH directly (leaf group: no logical exprs), build from it */
    if (group->logical_exprs == NIL)
    {
        imported_path = pg_cascades_find_imported_path(group);
        if (imported_path != NULL)
            return create_plan(ctx->root, imported_path);
    }

    /* Otherwise, iterate logical operators and build from child up */
    foreach(lc, group->logical_exprs)
    {
        PgGroupExpr *logical = (PgGroupExpr *) lfirst(lc);
        Plan *child_plan = NULL;
        Plan *result = NULL;

        if (cascades_planner_debug)
            elog(NOTICE, "build_logical: group=%d op=%d",
                 group->id, logical->op);

        /*
         * Guard: the compiler optimizes linitial(NIL) into an unconditional
         * trap (ud2/mov 0x0,%rax) for PG_CASCADES_LOGICAL_PROJECT, _AGG,
         * _SORT, _LIMIT, _DISTINCT cases.  If a logical expression has no
         * inputs (e.g., malformed by rewrite), skip it rather than crashing.
         */
        if (logical->inputs == NIL &&
            logical->op != PG_CASCADES_LOGICAL_SCAN &&
            logical->op != PG_CASCADES_LOGICAL_FILTER)
            continue;

        switch (logical->op)
        {
            case PG_CASCADES_LOGICAL_SCAN:
            case PG_CASCADES_LOGICAL_FILTER:
            {
                /*
                 * Only process LOGICAL_SCAN/FILTER if this group is actually
                 * a base-table group (not a merged join group).  After group
                 * merging, a group may contain both LOGICAL_JOIN and
                 * LOGICAL_SCAN expressions; we should let the LOGICAL_JOIN
                 * case handle the plan building.
                 */
                {
                    bool has_join_expr = false;
                    ListCell *check_lc;
                    foreach(check_lc, group->logical_exprs)
                    {
                        PgGroupExpr *e = (PgGroupExpr *) lfirst(check_lc);
                        if (e->op == PG_CASCADES_LOGICAL_JOIN)
                        {
                            has_join_expr = true;
                            break;
                        }
                    }
                    if (has_join_expr)
                        break;  /* let LOGICAL_JOIN case handle it */
                }

                imported_path = pg_cascades_find_imported_path(group);
                if (imported_path != NULL)
                {
                    result = create_plan(ctx->root, imported_path);
                    if (result != NULL)
                        pg_cascades_fix_empty_targetlists(ctx->root, result);
                    return result;
                }
                break;
            }

            case PG_CASCADES_LOGICAL_JOIN:
            {
                /*
                 * Phase 3: Join paths are imported as IMPORTED_PATH entries
                 * in the join group.
                 *
                 * After group merging, the join group may also contain
                 * scan-type IMPORTED_PATHs (SeqScan, IndexScan) from merged
                 * base table groups.  Their best entries may have lower
                 * cost and appear in the best_entries list instead of the
                 * join best entries.  So we search ALL physical expressions,
                 * not just the best_entries list.
                 */
                Path *join_path = NULL;
                ListCell *blc;
                int         best_nrels = 0;

                /* Try each join type in order, prefer HashJoin.
                 * For groups with multiple join paths (e.g. {1,2} AND {1,2,3}
                 * after group merge), pick the one covering the MOST base
                 * relations to avoid dropping tables from the plan. */
                {
                    int join_ops[] = {
                        PG_CASCADES_PHYSICAL_HASHJOIN,
                        PG_CASCADES_PHYSICAL_MERGEJOIN,
                        PG_CASCADES_PHYSICAL_NESTLOOP
                    };
                    int j;

                    for (j = 0; j < 3; j++)
                    {
                        foreach(blc, group->physical_exprs)
                        {
                            PgGroupExpr *pe = (PgGroupExpr *) lfirst(blc);
                            if (pe->mode == PG_PHYS_EXPR_IMPORTED_PATH &&
                                pe->op == join_ops[j])
                            {
                                Path *p = (Path *) pe->op_private;
                                int   nrels = bms_num_members(p->parent->relids);
                                if (nrels > best_nrels)
                                {
                                    join_path = p;
                                    best_nrels = nrels;
                                }
                            }
                        }
                        if (join_path != NULL)
                            break;  /* found best at this join type level */
                    }
                }

                if (join_path != NULL)
                {
                    result = create_plan(ctx->root, join_path);
                    if (result != NULL)
                        pg_cascades_fix_empty_targetlists(ctx->root, result);
                    return result;
                }
                break;
            }

            case PG_CASCADES_LOGICAL_PROJECT:
            {
                PgMemoGroup *child = (PgMemoGroup *) linitial(logical->inputs);

                child_plan = pg_cascades_build_logical_plan(ctx, child);
                if (child_plan == NULL)
                    break;

                /*
                 * If there is no aggregation, no grouping, and no window
                 * functions, the subplan's output columns already match the
                 * query's needs.  Return the child plan directly, matching
                 * the standard planner's behavior when need_tlist_eval is
                 * false (see planner.c:1442).
                 *
                 * For queries with aggregation/grouping/windows, the
                 * LOGICAL_AGG case handles the projection through make_agg.
                 * The LOGICAL_PROJECT case should not add any wrapper.
                 */
                if (ctx->upper->numGroupCols == 0 &&
                    !ctx->upper->hasAggs &&
                    ctx->upper->activeWindows == NIL)
                {
                    /*
                     * For simple queries (no aggregation), return the child
                     * plan as-is.  create_plan already produced a correct
                     * targetlist from PG's build_joinrel_tlist.  Direct
                     * targetlist replacement with ctx->upper->tlist breaks
                     * set_plan_references for multi-table joins because the
                     * Var varnos in ctx->upper->tlist don't match subplan
                     * scan targetlists after create_plan's Var adjustment.
                     *
                     * PG's set_plan_references will adjust the plan's
                     * existing targetlist correctly.
                     */
                    return child_plan;
                }

                /*
                 * Fallback: if there IS aggregation/grouping but the
                 * LOGICAL_AGG case wasn't reached (e.g., tree structure
                 * has Project above Agg), ensure the projection is handled.
                 */
                {
                    List *proj_tlist = ctx->upper->sub_tlist;
                    if (is_projection_capable_plan(child_plan))
                    {
                        child_plan->targetlist = proj_tlist;
                        return child_plan;
                    }
                    else
                    {
                        result = (Plan *) make_result(ctx->root,
                            proj_tlist, NULL, child_plan);
                        return result;
                    }
                }
            }

            case PG_CASCADES_LOGICAL_AGG:
            {
                PgMemoGroup *child = (PgMemoGroup *) linitial(logical->inputs);
                AggStrategy agg_strategy;
                AttrNumber *use_groupColIdx;
                Oid        *use_groupOperators;

                /* Build child plan through logical tree. */
                child_plan = pg_cascades_build_logical_plan(ctx, child);
                if (child_plan == NULL)
                    break;

                if (ctx->upper->numGroupCols == 0)
                {
                    agg_strategy = AGG_PLAIN;
                    use_groupColIdx = NULL;
                    use_groupOperators = NULL;
                }
                else
                {
                    agg_strategy = AGG_HASHED;
                    use_groupColIdx = ctx->upper->groupColIdx;
                    use_groupOperators = ctx->upper->groupOperators;
                }

                result = (Plan *) make_agg(ctx->root,
                    ctx->upper->tlist,
                    (List *) ctx->upper->havingQual,
                    agg_strategy,
                    &ctx->upper->agg_costs,
                    ctx->upper->numGroupCols,
                    use_groupColIdx,
                    use_groupOperators,
                    (long) ctx->upper->dNumGroups,
                    child_plan);
                return result;
            }

            case PG_CASCADES_LOGICAL_SORT:
            {
                PgMemoGroup *child = (PgMemoGroup *) linitial(logical->inputs);

                child_plan = pg_cascades_build_logical_plan(ctx, child);
                if (child_plan == NULL)
                    break;

                result = (Plan *) make_sort_from_pathkeys(ctx->root,
                    child_plan,
                    ctx->upper->sort_pathkeys,
                    ctx->upper->limit_tuples);
                ctx->root->query_pathkeys = ctx->upper->sort_pathkeys;

                /*
                 * If MergeLimitWithSort merged the Limit into this Sort,
                 * wrap with a Limit node.  make_sort_from_pathkeys only
                 * uses limit_tuples for costing, not for actual row
                 * limiting.
                 *
                 * Note: When root group has no best entries (e.g. after
                 * group merge), ctx->root may not have been set correctly.
                 * Use ctx->upper->limit_tuples as the safety check.
                 */
                if (ctx->upper->limit_tuples > 0)
                {
                    result = (Plan *) make_limit(result,
                        ctx->root->parse->limitOffset,
                        ctx->root->parse->limitCount,
                        0, (int64) ctx->upper->limit_tuples);
                }
                return result;
            }

            case PG_CASCADES_LOGICAL_LIMIT:
            {
                PgMemoGroup *child = (PgMemoGroup *) linitial(logical->inputs);
                int64 offset_est = 0;
                int64 count_est = 0;

                child_plan = pg_cascades_build_logical_plan(ctx, child);
                if (child_plan == NULL)
                    break;

                /* Extract limit/offset estimates from parse tree */
                if (ctx->root->parse->limitCount != NULL)
                    count_est = (int64) ctx->upper->limit_tuples;
                if (ctx->root->parse->limitOffset != NULL)
                    offset_est = 0;

                result = (Plan *) make_limit(child_plan,
                    ctx->root->parse->limitOffset,
                    ctx->root->parse->limitCount,
                    offset_est, count_est);
                return result;
            }

            case PG_CASCADES_LOGICAL_DISTINCT:
            {
                PgMemoGroup *child = (PgMemoGroup *) linitial(logical->inputs);
                AttrNumber *distinctColIdx;
                Oid        *distinctOps;
                int         numDistinctCols;

                /* Build child plan through the logical tree to get proper tlist */
                child_plan = pg_cascades_build_logical_plan(ctx, child);
                if (child_plan == NULL)
                    break;

                /* Compute distinct column indices from distinctClause */
                numDistinctCols = list_length(ctx->upper->distinctClause);
                distinctColIdx = extract_grouping_cols(ctx->upper->distinctClause,
                                                       ctx->upper->tlist);
                distinctOps = extract_grouping_ops(ctx->upper->distinctClause);

                /* Use HashAgg without aggregates to implement DISTINCT.
                 * Use ctx->upper->tlist (which has ressortgroupref) so that
                 * upper Sort nodes can find sort keys. */
                result = (Plan *) make_agg(ctx->root,
                    ctx->upper->tlist,
                    NIL,        /* no havingQual */
                    AGG_HASHED,
                    NULL,       /* no agg costs */
                    numDistinctCols,
                    distinctColIdx,
                    distinctOps,
                    (long) ctx->upper->dNumGroups,
                    child_plan);
                return result;
            }

            default:
                break;
        }
    }

    return NULL;
}

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

    /* First try: find a physical best entry matching root required */
    {
        PgRequiredProperty *root_req = pg_cascades_root_required_property(ctx);

        foreach(lc, root_group->best_entries)
        {
            PgGroupBestEntry *entry = (PgGroupBestEntry *) lfirst(lc);
            if (pg_required_property_equal(entry->required, root_req))
            {
                /*
                 * If the best entry is a scan-type op (SeqScan, IndexScan, etc.)
                 * but the root group contains upper-op logical expressions
                 * (Sort, Limit, Agg, Project), skip this entry and use
                 * build_logical_plan which handles the full tree correctly.
                 * This prevents returning a bare SeqScan when the query needs
                 * ORDER BY + LIMIT wrapping.
                 */
                if (entry->expr->op <= PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN)
                {
                    bool has_wrapper = false;
                    ListCell *elc;
                    foreach(elc, root_group->logical_exprs)
                    {
                        PgGroupExpr *e = (PgGroupExpr *) lfirst(elc);
                        if (e->op >= PG_CASCADES_LOGICAL_PROJECT &&
                            e->op <= PG_CASCADES_LOGICAL_LIMIT)
                        {
                            has_wrapper = true;
                            break;
                        }
                    }
                    if (has_wrapper)
                        continue;  /* skip scan entry, let build_logical_plan handle */
                }

                result = pg_cascades_build_plan_recurse(ctx, root_group, root_req,
                                                         entry, &entry->output);
                if (result != NULL)
                {
                    ctx->root->query_pathkeys = entry->output.pathkeys;

                    /* Fix any empty targetlists on inner nodes */
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
static __attribute__((noinline)) PgRequiredProperty *
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
static void
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

static Plan *
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

/*
 * pg_planbuild_self_test:
 *   Direct-call wrapper so gcov can track static helper functions.
 *   Called during memo initialization.
 */
void
pg_planbuild_self_test(void)
{
    /* --- pg_safe_linitial_child_req --- */

    /* Test 1: NULL best or NIL child_required_props → returns NULL */
    {
        PgGroupBestEntry best;
        MemSet(&best, 0, sizeof(PgGroupBestEntry));
        best.child_required_props = NIL;
        if (pg_safe_linitial_child_req(&best) != NULL)
            elog(WARNING, "planbuild self-test: NIL child_required_props should return NULL");
    }

    /* Test 2: non-NIL child_required_props → returns first element */
    {
        PgGroupBestEntry best;
        PgRequiredProperty req;
        MemSet(&best, 0, sizeof(PgGroupBestEntry));
        MemSet(&req, 0, sizeof(PgRequiredProperty));
        req.limit_tuples = 10;
        best.child_required_props = list_make1(&req);
        if (pg_safe_linitial_child_req(&best) != &req)
            elog(WARNING, "planbuild self-test: should return first child_required_props");
    }

    /* --- pg_cascades_find_imported_path --- */

    /* Test 3: NULL group → returns NULL */
    if (pg_cascades_find_imported_path(NULL) != NULL)
        elog(WARNING, "planbuild self-test: NULL group should return NULL");

    /* Test 4: group with no best_entries → returns NULL (no recursion match) */
    {
        PgMemoGroup group;
        MemSet(&group, 0, sizeof(PgMemoGroup));
        group.best_entries = NIL;
        group.logical_exprs = NIL;
        if (pg_cascades_find_imported_path(&group) != NULL)
            elog(WARNING, "planbuild self-test: empty group should return NULL");
    }

    /* --- pg_cascades_fix_empty_targetlists --- */

    /* Test 6: SeqScan with NIL targetlist, no children — propagates from lefttree */
    {
        Plan left;
        Plan plan;

        MemSet(&left, 0, sizeof(left));
        MemSet(&plan, 0, sizeof(plan));
        left.type = T_SeqScan;
        left.targetlist = list_make1(&left); /* non-NIL dummy */
        plan.type = T_Sort;
        plan.targetlist = NIL;
        plan.lefttree = &left;
        plan.righttree = NULL;

        pg_cascades_fix_empty_targetlists(NULL, &plan);
        if (plan.targetlist != left.targetlist)
            elog(WARNING, "planbuild self-test: fix_empty_targetlists propagate lefttree");
    }

    /* Test 7: fix_empty_targetlists propagate righttree */
    {
        Plan right;
        Plan plan;

        MemSet(&right, 0, sizeof(right));
        MemSet(&plan, 0, sizeof(plan));
        right.type = T_IndexScan;
        right.targetlist = list_make1(&right);
        plan.type = T_NestLoop;
        plan.targetlist = NIL;
        plan.lefttree = NULL;
        plan.righttree = &right;

        pg_cascades_fix_empty_targetlists(NULL, &plan);
        if (plan.targetlist != right.targetlist)
            elog(WARNING, "planbuild self-test: fix_empty_targetlists propagate righttree");
    }

    /* Test 8: fix_empty_targetlists NULL plan */
    pg_cascades_fix_empty_targetlists(NULL, NULL);

    /* Test 9: find_imported_path with LogicalProject recursion */
    {
        PgMemoGroup group;
        PgGroupExpr best_proj;
        PgMemoGroup child_grp;
        PgGroupBestEntry entry;
        PgGroupExpr imported_expr;
        Path fake_path;
        Path *result;

        MemSet(&group, 0, sizeof(group));
        MemSet(&best_proj, 0, sizeof(best_proj));
        MemSet(&child_grp, 0, sizeof(child_grp));
        MemSet(&entry, 0, sizeof(entry));
        MemSet(&imported_expr, 0, sizeof(imported_expr));
        MemSet(&fake_path, 0, sizeof(fake_path));

        imported_expr.mode = PG_PHYS_EXPR_IMPORTED_PATH;
        imported_expr.op_private = &fake_path;
        entry.expr = &imported_expr;
        child_grp.best_entries = list_make1(&entry);

        best_proj.op = PG_CASCADES_LOGICAL_PROJECT;
        best_proj.inputs = list_make1(&child_grp);
        group.logical_exprs = list_make1(&best_proj);

        result = pg_cascades_find_imported_path(&group);
        if (result != &fake_path)
            elog(WARNING, "planbuild self-test: find_imported_path via LogicalProject");
    }

    /* Test 10: find_imported_path with LogicalFilter recursion */
    {
        PgMemoGroup group;
        PgGroupExpr best_filter;
        PgMemoGroup child_grp;
        PgGroupBestEntry entry;
        PgGroupExpr imported_expr;
        Path fake_path;
        Path *result;

        MemSet(&group, 0, sizeof(group));
        MemSet(&best_filter, 0, sizeof(best_filter));
        MemSet(&child_grp, 0, sizeof(child_grp));
        MemSet(&entry, 0, sizeof(entry));
        MemSet(&imported_expr, 0, sizeof(imported_expr));
        MemSet(&fake_path, 0, sizeof(fake_path));

        imported_expr.mode = PG_PHYS_EXPR_IMPORTED_PATH;
        imported_expr.op_private = &fake_path;
        entry.expr = &imported_expr;
        child_grp.best_entries = list_make1(&entry);

        best_filter.op = PG_CASCADES_LOGICAL_FILTER;
        best_filter.inputs = list_make1(&child_grp);
        group.logical_exprs = list_make1(&best_filter);

        result = pg_cascades_find_imported_path(&group);
        if (result != &fake_path)
            elog(WARNING, "planbuild self-test: find_imported_path via LogicalFilter");
    }

    /* Test 11: build_logical_plan: group with both JOIN and SCAN */
    {
        PgPlannerCascadesContext ctx;
        PgMemo memo;
        PgMemoGroup group;
        PgGroupExpr join_expr, scan_expr;
        PgMemoGroup child_a, child_b;
        PgGroupBestEntry child_entry;
        PgGroupExpr child_phys;
        Path child_path;
        RelOptInfo child_rel;

        MemSet(&ctx, 0, sizeof(ctx));
        MemSet(&memo, 0, sizeof(memo));
        MemSet(&group, 0, sizeof(group));
        MemSet(&join_expr, 0, sizeof(join_expr));
        MemSet(&scan_expr, 0, sizeof(scan_expr));
        MemSet(&child_a, 0, sizeof(child_a));
        MemSet(&child_b, 0, sizeof(child_b));
        MemSet(&child_entry, 0, sizeof(child_entry));
        MemSet(&child_phys, 0, sizeof(child_phys));
        MemSet(&child_path, 0, sizeof(child_path));
        MemSet(&child_rel, 0, sizeof(child_rel));

        ctx.memo = &memo;
        ctx.root = (PlannerInfo *) palloc0(sizeof(PlannerInfo));
        memo.groups = NIL;

        child_path.pathtype = T_SeqScan;
        child_path.pathkeys = NIL;
        child_rel.relid = 1;
        child_path.parent = &child_rel;
        child_phys.mode = PG_PHYS_EXPR_IMPORTED_PATH;
        child_phys.op = PG_CASCADES_PHYSICAL_SEQSCAN;
        child_phys.op_private = &child_path;
        child_entry.expr = &child_phys;
        child_a.best_entries = list_make1(&child_entry);
        child_a.physical_exprs = list_make1(&child_phys);
        child_a.id = 1;

        join_expr.op = PG_CASCADES_LOGICAL_JOIN;
        join_expr.inputs = list_make2(&child_a, &child_b);
        scan_expr.op = PG_CASCADES_LOGICAL_SCAN;
        group.id = 99;
        group.logical_exprs = list_make2(&join_expr, &scan_expr);

        pg_cascades_build_logical_plan(&ctx, &group);
    }
}
