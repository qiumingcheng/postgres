/*-------------------------------------------------------------------------
 * cascades.c
 *    Cascades 优化器主入口：pg_cascades_try_grouping_planner,
 *    支持性检查, fallback 处理, GUC 变量定义
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/clauses.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "utils/memutils.h"
#include "catalog/pg_class.h"
#include "miscadmin.h"

/* ========================================================================
 * GUC 变量定义
 * ======================================================================== */

bool enable_cascades_planner = false;
bool cascades_planner_debug = false;
bool cascades_planner_fallback_on_error = false;
int  cascades_planner_timeout_ms = 0;
int  cascades_planner_max_groups = 10000;
int  cascades_planner_max_tasks = 100000;

/* ========================================================================
 * SubPlan 检测 (Phase 6b: 区分 correlated vs uncorrelated)
 * ======================================================================== */

/*
 * pg_cascades_contains_correlated_subplan:
 *   Walk expression tree looking for SubPlan nodes that are CORRELATED
 *   (parParam != NIL, meaning they reference outer query variables).
 *
 *   Uncorrelated SubPlans (initPlans, setParam != NIL, parParam == NIL)
 *   are safe — they execute once and return a constant.  The Cascades
 *   planner can treat them as opaque constants in the expression tree.
 *
 *   Only correlated SubPlans cause fallback, since they would need
 *   decorrelation to be properly optimized in the Memo.
 */
static bool
pg_cascades_contains_correlated_subplan_walker(Node *node, void *context)
{
    if (node == NULL)
        return false;

    if (IsA(node, SubPlan))
    {
        SubPlan *sp = (SubPlan *) node;

        /* Correlated: parParam is non-empty (references outer vars) */
        if (sp->parParam != NIL)
            return true;

        /* Uncorrelated initPlan — safe, continue walking */
        return expression_tree_walker(node,
                                       pg_cascades_contains_correlated_subplan_walker,
                                       context);
    }
    else if (IsA(node, AlternativeSubPlan))
    {
        /* AlternativeSubPlan wraps two SubPlans; check both */
        AlternativeSubPlan *asp = (AlternativeSubPlan *) node;
        ListCell *lc;

        foreach(lc, asp->subplans)
        {
            SubPlan *sp = (SubPlan *) lfirst(lc);
            if (sp->parParam != NIL)
                return true;
        }
        return false;
    }

    return expression_tree_walker(node,
                                   pg_cascades_contains_correlated_subplan_walker,
                                   context);
}

static bool
pg_cascades_contains_correlated_subplan(Node *node)
{
    return pg_cascades_contains_correlated_subplan_walker(node, NULL);
}

/* ========================================================================
 * 支持性检查
 * ======================================================================== */

PgCascadesStatus
pg_cascades_supported_query_precheck(PlannerInfo *root,
                                     PgCascadesUpperInfo *upper)
{
    Query *parse = root->parse;

    if (parse->commandType != CMD_SELECT)
        return PG_CASCADES_UNSUPPORTED;
    if (parse->setOperations)
    {
        SetOperationStmt *setOp = (SetOperationStmt *) parse->setOperations;

        /* UNION ALL: no dedup needed — allow Cascades to optimize leaf
         * queries.  PG's plan_set_operations() handles the top-level
         * Append plan.  UNION/INTERSECT/EXCEPT fall back (need dedup). */
        if (setOp->op != SETOP_UNION || !setOp->all)
            return PG_CASCADES_UNSUPPORTED_SETOP;
    }
    if (parse->hasWindowFuncs || upper->activeWindows != NIL)
        return PG_CASCADES_UNSUPPORTED_WINDOW;
    if (root->hasRecursion || parse->hasRecursive)
        return PG_CASCADES_UNSUPPORTED;
    if (parse->hasModifyingCTE)
        return PG_CASCADES_UNSUPPORTED;
    if (parse->rowMarks || root->rowMarks)
        return PG_CASCADES_UNSUPPORTED;
    if (parse->hasDistinctOn)
        return PG_CASCADES_UNSUPPORTED;
    if (root->minmax_aggs != NIL)
        return PG_CASCADES_UNSUPPORTED;
    if (pg_cascades_contains_correlated_subplan((Node *) parse->targetList) ||
        pg_cascades_contains_correlated_subplan((Node *) parse->jointree) ||
        pg_cascades_contains_correlated_subplan(parse->havingQual) ||
        pg_cascades_contains_correlated_subplan(parse->limitOffset) ||
        pg_cascades_contains_correlated_subplan(parse->limitCount))
        return PG_CASCADES_UNSUPPORTED_SUBPLAN;

    return PG_CASCADES_OK;
}

PgCascadesStatus
pg_cascades_supported_query(PlannerInfo *root, PgCascadesUpperInfo *upper)
{
    Index       rti;
    Query      *parse = root->parse;

    (void) upper;

    /*
     * Phase 1 Architectural Limitation: Complex JOIN queries with ORDER+LIMIT
     *
     * Root Cause (per 2.md analysis):
     * In Phase 1 Path-import mode, PG's make_one_rel() generates JOIN Paths,
     * but the join RelOptInfo->reltargetlist may be incomplete or empty at
     * this stage. When Cascades calls create_plan() on these Paths, the
     * resulting JOIN Plan has an empty targetlist, causing "variable not
     * found in subplan target lists" errors in set_plan_references().
     *
     * This is NOT a bug - it's an architectural limitation of Phase 1 where
     * Cascades depends on PG's lower planner structures that may not be
     * fully initialized for complex queries.
     *
     * Solution: Fallback to PG's standard planner for these queries.
     * Phase 2 will fix this by owning the complete logical tree.
     */
    if (parse->sortClause != NIL && parse->limitCount != NULL)
    {
        /* Count base relations from rtable (RTE_RELATION entries) */
        int num_base_rels = 0;
        ListCell *lc;

        foreach(lc, parse->rtable)
        {
            RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
            if (rte->rtekind == RTE_RELATION)
                num_base_rels++;
        }

        if (num_base_rels >= 3)
        {
            CASCADES_DEBUG(cascades_planner_debug, "Cascades: Phase 1 limitation - fallback for %d-table join with ORDER+LIMIT",
                     num_base_rels);
            return PG_CASCADES_UNSUPPORTED;
        }
    }

    for (rti = 1; rti < root->simple_rel_array_size; rti++)
    {
        RelOptInfo *rel = root->simple_rel_array[rti];
        RangeTblEntry *rte;

        if (rel == NULL)
            continue;
        if (rel->reloptkind != RELOPT_BASEREL)
            continue;

        rte = root->simple_rte_array[rti];

        if (rte->rtekind != RTE_RELATION)
            return PG_CASCADES_UNSUPPORTED_RTE_KIND;
        if (rte->inh)
            return PG_CASCADES_UNSUPPORTED_INHERITANCE;
        if (rte->relkind == RELKIND_FOREIGN_TABLE)
            return PG_CASCADES_UNSUPPORTED_FDW;
        if (rte->relkind != RELKIND_RELATION)
            return PG_CASCADES_UNSUPPORTED_RELKIND;
    }

    return PG_CASCADES_OK;
}

/* ========================================================================
 * Status / Fallback 处理
 * ======================================================================== */

void
pg_cascades_handle_status_or_error(PgCascadesStatus status, bool debug)
{
    if (status == PG_CASCADES_OK)
        return;

    /* Unsupported: always fallback */
    if (status >= PG_CASCADES_UNSUPPORTED &&
        status <= PG_CASCADES_UNSUPPORTED_SETOP)
    {
        if (debug)
            elog(NOTICE, "Cascades fallback: unsupported query (status=%d)",
                 status);
        return;
    }

    /* Internal errors: fallback only if GUC allows */
    if (cascades_planner_fallback_on_error)
    {
        if (debug)
            elog(WARNING, "Cascades fallback: internal error (status=%d)",
                 status);
        return;
    }

    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("Cascades planner internal error (status=%d)", status)));
}

/* ========================================================================
 * 主入口：pg_cascades_try_grouping_planner
 * ======================================================================== */

PgCascadesStatus
pg_cascades_try_grouping_planner(PlannerInfo *root,
                                 QueryPlannerPrepResult *prep,
                                 PgCascadesUpperInfo *upper,
                                 Plan **plan)
{
    PgPlannerCascadesContext ctx;
    PgCascadesStatus status;
    PgRule     *rules;
    MemoryContext old_cxt;

    MemSet(&ctx, 0, sizeof(PgPlannerCascadesContext));

    CASCADES_DEBUG(cascades_planner_debug, "CASCADES: ====== BEGIN optimization ======");

    /* 1. Create Cascades MemoryContext */
    ctx.memo_cxt = AllocSetContextCreate(root->planner_cxt,
                                         "PgCascadesMemo",
                                         ALLOCSET_DEFAULT_MINSIZE,
                                         ALLOCSET_DEFAULT_INITSIZE,
                                         ALLOCSET_DEFAULT_MAXSIZE);
    old_cxt = MemoryContextSwitchTo(ctx.memo_cxt);
    CASCADES_DEBUG(cascades_planner_debug, "CASCADES: [1/12] MemoryContext created");

    /* 2a. Initialize rule patterns (Phase 5: multi-node pattern matching) */
    pg_cascades_init_rule_patterns();

    /* 2. Initialize context */
    ctx.root = root;
    ctx.upper = upper;
    ctx.prep = prep;
    ctx.max_groups = cascades_planner_max_groups;
    ctx.max_tasks = cascades_planner_max_tasks;
    ctx.timeout_ms = cascades_planner_timeout_ms;
    ctx.start_time = GetCurrentTimestamp();
    ctx.debug = cascades_planner_debug;
    ctx.task_stack = NIL;
    ctx.fallback_reasons = NIL;
    ctx.upper_bound_cost = 0;  /* Phase 4 */

    /* 3. Set up rules (Phase 4: sorted by promise descending) */
    {
        int num_impl, num_trans;

        /*
         * Merge: Phase 1 impl + Phase 2 scan/join (COMPOSABLE_OP) +
         * Phase 4 join (make_join_rel) + Enforcer rules.
         *
         * Phase 2 scan rules (bits 6-8) create COMPOSABLE_OP physical
         * scan expressions.  pg_derive_child_properties now handles
         * COMPOSABLE_OP safely (checks mode before casting op_private).
         * These rules give the task scheduler alternative physical
         * implementations to cost and compare.
         */
        rules = pg_cascades_get_impl_rules(&num_impl);
        {
            PgRule *enf_rules, *join_rules, *scan_rules, *join2_rules;
            int     num_enforcer, num_join, num_scan, num_join2;
            int     total;
            PgRule *merged;

            enf_rules = pg_cascades_get_enforcer_rules(&num_enforcer);
            join_rules = pg_cascades_get_impl_rules_phase4_join(&num_join);
            scan_rules = pg_cascades_get_impl_rules_phase2_scan(&num_scan);
            join2_rules = pg_cascades_get_impl_rules_phase2_join(&num_join2);
            total = num_impl + num_join + num_scan + num_join2 + num_enforcer;
            merged = (PgRule *) palloc(sizeof(PgRule) * (total + 1));

            if (num_impl > 0)
                memcpy(merged, rules, sizeof(PgRule) * num_impl);
            if (num_join > 0)
                memcpy(&merged[num_impl], join_rules, sizeof(PgRule) * num_join);
            if (num_scan > 0)
                memcpy(&merged[num_impl + num_join], scan_rules,
                       sizeof(PgRule) * num_scan);
            if (num_join2 > 0)
                memcpy(&merged[num_impl + num_join + num_scan], join2_rules,
                       sizeof(PgRule) * num_join2);
            if (num_enforcer > 0)
                memcpy(&merged[num_impl + num_join + num_scan + num_join2],
                       enf_rules, sizeof(PgRule) * num_enforcer);
            MemSet(&merged[total], 0, sizeof(PgRule));

            ctx.impl_rules = pg_cascades_get_rules_sorted(merged, &total);
            ctx.num_impl_rules = total;
            pfree(merged);
        }

        /* Merge Phase 3 + Phase 5 transformation rules */
        rules = pg_cascades_get_trans_rules(&num_trans);
        {
            PgRule *phase5_rules;
            int     num_phase5;

            phase5_rules = pg_cascades_get_trans_rules_phase5(&num_phase5);
            if (num_phase5 > 0)
            {
                int total = num_trans + num_phase5;
                PgRule *merged = (PgRule *) palloc(sizeof(PgRule) * (total + 1));

                if (num_trans > 0)
                    memcpy(merged, rules, sizeof(PgRule) * num_trans);
                memcpy(&merged[num_trans], phase5_rules,
                       sizeof(PgRule) * num_phase5);
                MemSet(&merged[total], 0, sizeof(PgRule)); /* sentinel */

                ctx.trans_rules = pg_cascades_get_rules_sorted(merged, &total);
                ctx.num_trans_rules = total;
            }
            else if (num_trans > 0)
            {
                ctx.trans_rules = pg_cascades_get_rules_sorted(rules, &num_trans);
                ctx.num_trans_rules = num_trans;
            }
            else
            {
                ctx.trans_rules = NULL;
                ctx.num_trans_rules = 0;
            }
        }
    }

    CASCADES_DEBUG(cascades_planner_debug, "CASCADES: [2/12] Rules initialized (impl=%d trans=%d)",
             ctx.num_impl_rules, ctx.num_trans_rules);

    /* 4. Handle trivial_result */
    if (prep->trivial_result)
    {
        MemoryContextSwitchTo(old_cxt);
        MemoryContextDelete(ctx.memo_cxt);
        return PG_CASCADES_UNSUPPORTED;
    }

    /* 5. Build lower paths if not already built.
     * For ≤8 tables: PG's standard_join_search provides exhaustive DP.
     * For >8 tables: GEQO provides an initial plan; Cascades then tries
     *   to improve it via Phase4 rules + JoinAssociativity + bound pruning.
     *   This is a StarRocks MultiJoinBinder-equivalent relay: GEQO sets
     *   the upper bound, Cascades searches for local improvements. */
    if (!prep->lower_paths_built)
    {
        RelOptInfo *final_rel;

        /*
         * CRITICAL: Switch back to the caller's memory context before
         * calling make_one_rel.  PG's standard planner allocates
         * RelOptInfo, Path, and related structures in the current
         * memory context.  If we stay in ctx.memo_cxt, all of these
         * go into the memo context and get freed when we call
         * MemoryContextDelete below.  The fallback path
         * (finish_query_planner_after_prepare) then accesses
         * freed memory → SIGSEGV.
         */
        {
            MemoryContext save_cxt = MemoryContextSwitchTo(old_cxt);

            PG_TRY();
            {
                final_rel = make_one_rel(root, prep->joinlist);
                prep->lower_paths_built = true;
                prep->final_rel = final_rel;
            }
            PG_CATCH();
            {
                MemoryContextSwitchTo(old_cxt);
                MemoryContextDelete(ctx.memo_cxt);
                PG_RE_THROW();
            }
            PG_END_TRY();

            MemoryContextSwitchTo(save_cxt);
        }
    }

    if (prep->final_rel == NULL ||
        prep->final_rel->cheapest_total_path == NULL)
    {
        MemoryContextSwitchTo(old_cxt);
        MemoryContextDelete(ctx.memo_cxt);
        CASCADES_DEBUG(cascades_planner_debug, "CASCADES: [3/12] make_one_rel FAILED — no valid paths");
        return PG_CASCADES_INTERNAL_NO_PLAN;
    }

    CASCADES_DEBUG(cascades_planner_debug, "CASCADES: [3/12] make_one_rel OK (final_rel rows=%.0f width=%d)",
             prep->final_rel->rows, prep->final_rel->width);

    /* 6. Build Memo from Query tree (StarRocks: Memo.init) */
    pg_memo_init_from_tree(&ctx);

    if (ctx.memo->root_group == NULL)
    {
        MemoryContextSwitchTo(old_cxt);
        MemoryContextDelete(ctx.memo_cxt);
        CASCADES_DEBUG(cascades_planner_debug, "CASCADES: [4/12] Memo init FAILED — no root group");
        return PG_CASCADES_INTERNAL_NO_PLAN;
    }

    CASCADES_DEBUG(cascades_planner_debug, "CASCADES: [4/12] Memo init OK (%d groups, root=%d)",
             list_length(ctx.memo->groups), ctx.memo->root_group->id);

    /* 7. Derive logical property */
    pg_memo_derive_logical_property(ctx.memo, ctx.memo->root_group, &ctx);
    CASCADES_DEBUG(cascades_planner_debug, "CASCADES: [5/12] Logical property derived");

    /*
     * 7c. Phase 4: Run staged + combination-rule rewrite on Memo groups.
     * Operates on ctx->memo->root_group.
     */
    pg_cascades_logical_rewrite(&ctx);

    /* Re-derive logical properties after rewrite */
    pg_memo_derive_logical_property_v2(ctx.memo, &ctx);

    /*
     * Phase 6: After rewrite, root_group may point to an empty group
     * (merged away by EliminateLimit/EliminateProject).  Find the
     * first non-empty group and make it the new root.
     */
    {
        PgMemoGroup *root_g = ctx.memo->root_group;
        if (root_g != NULL &&
            root_g->logical_exprs == NIL &&
            root_g->physical_exprs == NIL)
        {
            ListCell *lc;
            PgMemoGroup *best = NULL;
            /* Find the LAST non-empty group: outermost wrappers
             * (Limit, Agg, Project) are added last. */
            foreach(lc, ctx.memo->groups)
            {
                PgMemoGroup *g = (PgMemoGroup *) lfirst(lc);
                if (g->logical_exprs != NIL || g->physical_exprs != NIL)
                    best = g;
            }
            if (best != NULL)
            {
                ctx.memo->root_group = best;
                CASCADES_DEBUG(ctx.debug, "Cascades: updated root_group from %d to %d after rewrite",
                         root_g->id, best->id);
            }
        }
    }

    CASCADES_DEBUG(cascades_planner_debug, "CASCADES: [6/12] Rewrite done (%d groups after rewrite)",
             list_length(ctx.memo->groups));

    /* 8. Run task scheduler */
    {
        PgOptimizerTask *root_task;
        ListCell       *lc_go;
        foreach(lc_go, ctx.memo->groups)
            ((PgMemoGroup *) lfirst(lc_go))->optimized = false;
        root_task = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
        root_task->type = PG_TASK_OPTIMIZE_GROUP;
        root_task->group = ctx.memo->root_group;
        task_stack_push(&ctx, root_task);
    }

    status = pg_cascades_run_tasks(&ctx);

    CASCADES_DEBUG(cascades_planner_debug, "CASCADES: [7/12] Task scheduler done (tasks=%d, upper_bound=%.2f, status=%d)",
             ctx.num_tasks_executed, ctx.upper_bound_cost, status);

    if (cascades_planner_debug)
    {
        debug_print_cascades_memo(&ctx);
        debug_print_cascades_rules(&ctx);
    }

    /* 9. Extract best plan */
    if (status == PG_CASCADES_OK)
    {
        *plan = pg_cascades_extract_best_plan(&ctx);
        if (*plan == NULL)
        {
            status = PG_CASCADES_INTERNAL_NO_PLAN;
            CASCADES_DEBUG(cascades_planner_debug, "CASCADES: [8/12] Plan extraction FAILED — no valid plan");
        }
        else CASCADES_DEBUG(cascades_planner_debug, "CASCADES: [8/12] Plan extraction OK (plan_type=%d)",
                 (*plan)->type);
    }

    /*
     * 9b. Phase 6: Post-optimization — validate and rewrite the plan.
     * - Validate structural integrity (pg_cascades_validate_plan)
     * - Insert Material nodes on NestLoop inner sides where needed
     *   (pg_cascades_physical_rewrite)
     */
    if (status == PG_CASCADES_OK && *plan != NULL)
    {
        pg_cascades_validate_plan(*plan);
        CASCADES_DEBUG(cascades_planner_debug, "CASCADES: [9/12] Plan validated");
        *plan = pg_cascades_physical_rewrite(&ctx, *plan);
        CASCADES_DEBUG(cascades_planner_debug, "CASCADES: [10/12] Physical rewrite done");
    }

    /* 10. Cleanup - copy plan out before deleting memo context */
    if (*plan != NULL)
    {
        MemoryContextSwitchTo(old_cxt);
        *plan = copyObject(*plan);
    }
    MemoryContextSwitchTo(old_cxt);
    MemoryContextDelete(ctx.memo_cxt);

    {
        long secs;
        int  microsecs;
        TimestampDifference(ctx.start_time, GetCurrentTimestamp(),
                            &secs, &microsecs);
        CASCADES_DEBUG(cascades_planner_debug, "CASCADES: [11/12] Cleanup done (elapsed=%.1fms)",
                 secs * 1000.0 + microsecs / 1000.0);
    }
    CASCADES_DEBUG(cascades_planner_debug, "CASCADES: [12/12] ====== END optimization (status=%d) ======", status);

    return status;
}
