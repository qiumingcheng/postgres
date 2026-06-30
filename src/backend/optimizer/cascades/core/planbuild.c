/*-------------------------------------------------------------------------
 * planbuild.c
 *    Cascades Plan Builder: 从 Memo best 表抽取最优 Plan *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "optimizer/planmain.h"
#include "core/registry.h"
#include "core/registry.h"
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

    elog(NOTICE, "PLANBUILD_TRACE: root_group=%d best_entries=%d logical=%d physical=%d",
         root_group->id, list_length(root_group->best_entries),
         list_length(root_group->logical_exprs), list_length(root_group->physical_exprs));

    /*
     * Fix: after Final Cleanup group merge, root_group may be the merge
     * source (empty).  Find the last non-empty group with best_entries
     * and use it as root for extraction. */
    if (list_length(root_group->best_entries) == 0)
    {
        foreach(lc, ctx->memo->groups)
        {
            PgMemoGroup *g = (PgMemoGroup *) lfirst(lc);
            if (list_length(g->best_entries) > 0 && g != root_group)
                root_group = g;
        }
    }

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
                elog(NOTICE, "PLANBUILD_TRACE: pass=%d best op=%d mode=%d req_pk=%p out_rows=%.0f",
                     pass, entry->expr->op, entry->expr->mode,
                     entry->required->pathkeys, entry->output.rows);
                if (!pg_required_property_equal(entry->required, root_req))
                {
                    elog(NOTICE, "PLANBUILD_TRACE: SKIP — required property mismatch "
                         "(entry req_pk=%p vs root_req pk=%p)",
                         entry->required->pathkeys, root_req->pathkeys);
                    continue;
                }

        /* Pass 0: COMPOSABLE_OP only; Pass 1: IMPORTED_PATH only */
                if (pass == 0 && entry->expr->mode == PG_PHYS_EXPR_IMPORTED_PATH)
                {
                    elog(NOTICE, "PLANBUILD_TRACE: SKIP — pass=0 but IMPORTED_PATH");
                    continue;
                }
                if (pass == 1 && entry->expr->mode != PG_PHYS_EXPR_IMPORTED_PATH)
                {
                    elog(NOTICE, "PLANBUILD_TRACE: SKIP — pass=1 but COMPOSABLE_OP");
                    continue;
                }

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
                elog(NOTICE, "PLANBUILD_TRACE: build_plan_recurse returned %s",
                     result ? "Plan*" : "NULL");
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
    /*
     * Group merge may leave root_group with best entries that don't
     * match root_req exactly (e.g., cross-joins after Final Cleanup).
     * Fall back to the first available best entry as root. */
    if (list_length(root_group->best_entries) > 0)
    {
        PgGroupBestEntry *entry = (PgGroupBestEntry *)
            linitial(root_group->best_entries);
        result = pg_cascades_build_plan_recurse(ctx, root_group,
            entry->required, entry, &entry->output);
        if (result != NULL)
        {
            ctx->root->query_pathkeys = entry->output.pathkeys;
            pg_cascades_fix_empty_targetlists(ctx->root, result);
            return result;
        }
    }

    CASCADES_DEBUG(cascades_planner_debug,
        "CASCADES: plan extraction FAILED — no best_entries in root group %d",
        root_group->id);
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
    CASCADES_DEBUG(cascades_planner_debug,
        "CASCADES: fix_empty_targetlists node=%d tlist_len=%d",
        (int) nodeTag(plan), list_length(plan->targetlist));

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
        CASCADES_DEBUG(cascades_planner_debug, "fix_empty_targetlists: node type %d has NIL targetlist, attempting fix",
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
            CASCADES_DEBUG(cascades_planner_debug, "fix_empty_targetlists: fixed node type %d using lefttree targetlist",
                     (int) nodeTag(plan));
            return;
        }
        if (plan->righttree != NULL && plan->righttree->targetlist != NIL)
        {
            plan->targetlist = plan->righttree->targetlist;
            CASCADES_DEBUG(cascades_planner_debug, "fix_empty_targetlists: fixed node type %d using righttree targetlist",
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

    /* vtable dispatch: only Upper Ops (HASHAGG+) — scan/join use switch */
    if (expr->op >= PG_CASCADES_PHYSICAL_HASHAGG)
    {
        PgOperatorVtable *vt = pg_registry_get_vtable(expr->op);
        if (vt && vt->build_plan_fn)
        {
            Plan *p = vt->build_plan_fn(ctx, group, expr, best, required, output);
            if (p) return p;
        }
    }

    switch (expr->op)
    {
        /*
         * Scan / Join ops: prefer IMPORTED_PATH (PG's battle-tested paths).
         *
         * For COMPOSABLE_OP, delegate to an IMPORTED_PATH entry of the same
         * op type in the same group.  COMPOSABLE_OP join entries are created
         * by implementation rules for costing purposes — the group always
         * has a corresponding IMPORTED_PATH entry from make_one_rel().
         *
         * StarRocks alignment: In StarRocks all entries are COMPOSABLE_OP
         * and plan extraction recurses into child groups directly.  In PG,
         * IMPORTED_PATH entries carry a complete Path tree that create_plan
         * handles natively — delegating is both simpler and safer.
         */
        case PG_CASCADES_PHYSICAL_SEQSCAN:
        case PG_CASCADES_PHYSICAL_INDEXSCAN:
        case PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN:
        case PG_CASCADES_PHYSICAL_NESTLOOP:
        case PG_CASCADES_PHYSICAL_HASHJOIN:
        case PG_CASCADES_PHYSICAL_MERGEJOIN:
            if (expr->mode == PG_PHYS_EXPR_IMPORTED_PATH)
            {
                result = create_plan(ctx->root, (Path *) expr->op_private);
                if (result != NULL)
                    pg_cascades_fix_empty_targetlists(ctx->root, result);
                break;
            }
            /* COMPOSABLE_OP: delegate to IMPORTED_PATH in same group */
            {
                ListCell *pe;
                foreach(pe, group->physical_exprs)
                {
                    PgGroupExpr *e = (PgGroupExpr *) lfirst(pe);
                    if (e->mode == PG_PHYS_EXPR_IMPORTED_PATH &&
                        e->op == expr->op)
                    {
                        result = create_plan(ctx->root, (Path *) e->op_private);
                        if (result != NULL)
                        {
                            pg_cascades_fix_empty_targetlists(ctx->root, result);
                            elog(NOTICE, "PLANBUILD_JOIN: COMPOSABLE_OP %d delegated to IMPORTED_PATH",
                                 expr->op);
                            break;
                        }
                    }
                }
                /* If no IMPORTED_PATH found, this group has no PG paths
                 * (e.g., a Phase 2 join from JoinAssociativity).
                 * Future work: implement full COMPOSABLE_OP join extraction
                 * via recursive child plan building. */
            }
            break;

        /* Upper ops (HASHAGG+) handled by vtable dispatch above.
         * If vtable returned NULL, return NULL to trigger fallback. */

        default:
            return NULL;
            break;
    }

    return result;
}
