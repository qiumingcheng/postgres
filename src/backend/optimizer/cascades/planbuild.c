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

        switch (logical->op)
        {
            case PG_CASCADES_LOGICAL_JOIN:
            {
                /*
                 * Phase 3: Join paths are imported as IMPORTED_PATH entries
                 * in the join group. Find the best matching entry and build
                 * the plan from the Path directly.
                 */
                PgGroupBestEntry *best = NULL;
                ListCell *blc;

                /* Find best entry in this group */
                foreach(blc, group->best_entries)
                {
                    PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(blc);
                    if (e->expr->mode == PG_PHYS_EXPR_IMPORTED_PATH)
                    {
                        best = e;
                        break;
                    }
                }

                if (best != NULL)
                {
                    result = create_plan(ctx->root, (Path *) best->expr->op_private);
                    return result;
                }
                break;
            }

            case PG_CASCADES_LOGICAL_PROJECT:
            {
                PgMemoGroup *child = (PgMemoGroup *) linitial(logical->inputs);
                List *proj_tlist;

                child_plan = pg_cascades_build_logical_plan(ctx, child);
                if (child_plan == NULL)
                    break;

                /*
                 * Use sub_tlist (no Aggrefs) for projection below upper ops.
                 * sub_tlist provides the base columns needed by Agg/Sort etc.
                 * For non-Agg queries, sub_tlist ≈ tlist so this is safe.
                 */
                proj_tlist = ctx->upper->sub_tlist;

                result = (Plan *) make_result(ctx->root,
                    proj_tlist,
                    NULL, /* resconstantqual */
                    child_plan);
                return result;
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
                result = pg_cascades_build_plan_recurse(ctx, root_group, root_req,
                                                         entry, &entry->output);
                if (result != NULL)
                {
                    ctx->root->query_pathkeys = entry->output.pathkeys;

                    /* Phase 4h: validate plan structure */
                    pg_cascades_validate_plan(result);

                    /* Phase 4g: post-optimization physical rewrite */
                    result = pg_cascades_physical_rewrite(ctx, result);

                    return result;
                }
            }
        }
    }

    /*
     * Fallback: build plan recursively from logical expressions tree.
     */
    result = pg_cascades_build_logical_plan(ctx, root_group);
    if (result != NULL)
    {
        pg_cascades_validate_plan(result);
        result = pg_cascades_physical_rewrite(ctx, result);
        return result;
    }

    if (cascades_planner_debug)
        elog(WARNING, "Cascades: no plan found");
    return NULL;
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
            result = create_plan(ctx->root, (Path *) expr->op_private);
            break;

        /* === Upper Ops: recursive build + wrap === */

        case PG_CASCADES_PHYSICAL_SORT:
            {
                Plan *child;
                PgGroupBestEntry *child_best = NULL;
                PgOutputProperty child_out;
                ListCell *lc;

                /* find child best */
                foreach(lc, ((PgMemoGroup *)linitial(expr->inputs))->best_entries)
                {
                    PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
                    if (best->child_required_props != NIL &&
                        pg_required_property_equal(e->required,
                            (PgRequiredProperty *) linitial(best->child_required_props)))
                    {
                        child_best = e;
                        break;
                    }
                }
                if (child_best == NULL)
                    return NULL;

                child = pg_cascades_build_plan_recurse(ctx,
                    (PgMemoGroup *) linitial(expr->inputs),
                    (PgRequiredProperty *) linitial(best->child_required_props),
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

                child_group = (PgMemoGroup *) linitial(expr->inputs);

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
                            (PgRequiredProperty *) linitial(best->child_required_props)))
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
                    (PgRequiredProperty *) linitial(best->child_required_props),
                    child_best, &child_out);
                if (child == NULL)
                {
                    if (cascades_planner_debug)
                        elog(WARNING, "planbuild HashAgg: child plan is NULL");
                    return NULL;
                }

                result = (Plan *) make_agg(ctx->root,
                    ctx->upper->tlist,
                    (List *) ctx->upper->havingQual,
                    AGG_HASHED,
                    &ctx->upper->agg_costs,
                    ctx->upper->numGroupCols,
                    ctx->upper->groupColIdx,
                    ctx->upper->groupOperators,
                    (long) ctx->upper->dNumGroups,
                    child);
            }
            break;

        case PG_CASCADES_PHYSICAL_GROUPAGG:
            {
                Plan *child;
                PgGroupBestEntry *child_best = NULL;
                PgOutputProperty child_out;
                ListCell *lc;

                foreach(lc, ((PgMemoGroup *)linitial(expr->inputs))->best_entries)
                {
                    PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
                    if (best->child_required_props != NIL &&
                        pg_required_property_equal(e->required,
                            (PgRequiredProperty *) linitial(best->child_required_props)))
                    {
                        child_best = e;
                        break;
                    }
                }
                if (child_best == NULL)
                    return NULL;

                child = pg_cascades_build_plan_recurse(ctx,
                    (PgMemoGroup *) linitial(expr->inputs),
                    (PgRequiredProperty *) linitial(best->child_required_props),
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
                ListCell *lc;

                foreach(lc, ((PgMemoGroup *)linitial(expr->inputs))->best_entries)
                {
                    PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
                    if (best->child_required_props != NIL &&
                        pg_required_property_equal(e->required,
                            (PgRequiredProperty *) linitial(best->child_required_props)))
                    {
                        child_best = e;
                        break;
                    }
                }
                if (child_best == NULL)
                    return NULL;

                child = pg_cascades_build_plan_recurse(ctx,
                    (PgMemoGroup *) linitial(expr->inputs),
                    (PgRequiredProperty *) linitial(best->child_required_props),
                    child_best, &child_out);

                result = (Plan *) make_unique(child, ctx->upper->distinctClause);
            }
            break;

        case PG_CASCADES_PHYSICAL_LIMIT:
            {
                Plan *child;
                PgGroupBestEntry *child_best = NULL;
                PgOutputProperty child_out;
                ListCell *lc;

                foreach(lc, ((PgMemoGroup *)linitial(expr->inputs))->best_entries)
                {
                    PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
                    if (best->child_required_props != NIL &&
                        pg_required_property_equal(e->required,
                            (PgRequiredProperty *) linitial(best->child_required_props)))
                    {
                        child_best = e;
                        break;
                    }
                }
                if (child_best == NULL)
                    return NULL;

                child = pg_cascades_build_plan_recurse(ctx,
                    (PgMemoGroup *) linitial(expr->inputs),
                    (PgRequiredProperty *) linitial(best->child_required_props),
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
                ListCell *lc;

                foreach(lc, ((PgMemoGroup *)linitial(expr->inputs))->best_entries)
                {
                    PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
                    if (best->child_required_props != NIL &&
                        pg_required_property_equal(e->required,
                            (PgRequiredProperty *) linitial(best->child_required_props)))
                    {
                        child_best = e;
                        break;
                    }
                }
                if (child_best == NULL)
                    return NULL;

                child = pg_cascades_build_plan_recurse(ctx,
                    (PgMemoGroup *) linitial(expr->inputs),
                    (PgRequiredProperty *) linitial(best->child_required_props),
                    child_best, &child_out);

                /* Wrap with Result. Use sub_tlist (no Aggrefs) for safety. */
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
