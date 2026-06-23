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
        return PG_CASCADES_UNSUPPORTED_SETOP;
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

    (void) upper;

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

    /* 1. Create Cascades MemoryContext */
    ctx.memo_cxt = AllocSetContextCreate(root->planner_cxt,
                                         "PgCascadesMemo",
                                         ALLOCSET_DEFAULT_MINSIZE,
                                         ALLOCSET_DEFAULT_INITSIZE,
                                         ALLOCSET_DEFAULT_MAXSIZE);
    old_cxt = MemoryContextSwitchTo(ctx.memo_cxt);

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
        int num_impl, num_trans, num_enforcer;

        /*
         * Merge: Phase 1 impl + Enforcer rules.
         *
         * Note: Phase 2 scan rules (bits 6-8) are NOT merged here.
         * In the tree-based Memo (Phase 6), LogicalScan groups already
         * have IMPORTED_PATH physical expressions (with real PG Path*).
         * Re-running scan impl rules would create COMPOSABLE_OP scan
         * expressions whose op_private is RelOptInfo* (not Path*),
         * causing SIGSEGV in pg_derive_child_properties.
         *
         * Phase 2 join rules (bits 9-11) + Phase 4 path-generation join
         * rules (bits 43-45) remain deferred to Phase 6.
         */
        rules = pg_cascades_get_impl_rules(&num_impl);
        {
            PgRule *enf_rules;
            int     total_p1_enforcer;
            PgRule *merged_p1_enforcer;

            /* Merge: Phase 1 + Enforcer */
            enf_rules = pg_cascades_get_enforcer_rules(&num_enforcer);
            total_p1_enforcer = num_impl + num_enforcer;
            merged_p1_enforcer = (PgRule *) palloc(sizeof(PgRule) * (total_p1_enforcer + 1));

            if (num_impl > 0)
                memcpy(merged_p1_enforcer, rules, sizeof(PgRule) * num_impl);
            if (num_enforcer > 0)
                memcpy(&merged_p1_enforcer[num_impl], enf_rules,
                       sizeof(PgRule) * num_enforcer);
            MemSet(&merged_p1_enforcer[total_p1_enforcer], 0, sizeof(PgRule));

            ctx.impl_rules = pg_cascades_get_rules_sorted(
                merged_p1_enforcer, &total_p1_enforcer);
            ctx.num_impl_rules = total_p1_enforcer;
            pfree(merged_p1_enforcer);
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

    /* 4. Handle trivial_result */
    if (prep->trivial_result)
    {
        MemoryContextSwitchTo(old_cxt);
        MemoryContextDelete(ctx.memo_cxt);
        return PG_CASCADES_UNSUPPORTED;
    }

    /* 5. Build lower paths if not already built */
    if (!prep->lower_paths_built)
    {
        RelOptInfo *final_rel;

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
    }

    if (prep->final_rel == NULL ||
        prep->final_rel->cheapest_total_path == NULL)
    {
        MemoryContextSwitchTo(old_cxt);
        MemoryContextDelete(ctx.memo_cxt);
        if (cascades_planner_debug)
            elog(NOTICE, "Cascades: no valid lower paths");
        return PG_CASCADES_INTERNAL_NO_PLAN;
    }

    /*
     * 6. Build Memo from OptExpression tree (Phase 6).
     *
     * The standalone tree (from pg_cascades_build_initial_tree) has a
     * multi-level logical structure: Limit → Sort → Agg → Project → Scan.
     * pg_memo_insert_expression_tree converts it to proper Memo groups,
     * and for LogicalScan nodes with valid RelOptInfo* op_private, imports
     * PG paths as physical candidates and sets group->rel to avoid SIGSEGV.
     *
     * Previous Phase 2/3 path-import approach (pg_cascades_build_logical_root)
     * is replaced — the tree-based Memo gives rewrite rules a structured
     * multi-level logical tree to walk and transform.
     */
    {
        PgMemo     *memo;
        HASHCTL     hash_ctl;
        PgGroupExpr *opt_tree;

        /* Create minimal Memo shell */
        {
            MemoryContext old_cxt2 = MemoryContextSwitchTo(ctx.memo_cxt);

            memo = (PgMemo *) palloc0(sizeof(PgMemo));
            memo->context = ctx.memo_cxt;
            memo->groups = NIL;

            MemSet(&hash_ctl, 0, sizeof(hash_ctl));
            hash_ctl.keysize = sizeof(PgExprHashKey);
            hash_ctl.entrysize = sizeof(PgExprHashKey);
            hash_ctl.hcxt = ctx.memo_cxt;
            memo->group_expr_table = hash_create("Memo GroupExpr Table", 256,
                                                  &hash_ctl,
                                                  HASH_ELEM | HASH_CONTEXT);

            ctx.memo = memo;
            MemoryContextSwitchTo(old_cxt2);
        }

        /* Build tree and insert into Memo */
        opt_tree = pg_cascades_build_initial_tree(&ctx);
        pg_memo_insert_expression_tree(&ctx, opt_tree);
    }

    if (ctx.memo->root_group == NULL)
    {
        MemoryContextSwitchTo(old_cxt);
        MemoryContextDelete(ctx.memo_cxt);
        return PG_CASCADES_INTERNAL_NO_PLAN;
    }

    /* 7. Derive logical property */
    pg_memo_derive_logical_property(ctx.memo, ctx.memo->root_group, &ctx);

    /*
     * 7c. Phase 4: Run staged + combination-rule rewrite on Memo groups.
     * Operates on ctx->memo->root_group.
     */
    pg_cascades_logical_rewrite(&ctx);

    /* Re-derive logical properties after rewrite */
    pg_memo_derive_logical_property_v2(ctx.memo, &ctx);

    /* 8. Run task scheduler */
    {
        PgOptimizerTask *root_task;

        root_task = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
        root_task->type = PG_TASK_OPTIMIZE_GROUP;
        root_task->group = ctx.memo->root_group;
        task_stack_push(&ctx, root_task);
    }

    status = pg_cascades_run_tasks(&ctx);

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
            status = PG_CASCADES_INTERNAL_NO_PLAN;
    }

    /* 10. Cleanup - copy plan out before deleting memo context */
    if (*plan != NULL)
    {
        MemoryContextSwitchTo(old_cxt);
        *plan = copyObject(*plan);
    }
    MemoryContextSwitchTo(old_cxt);
    MemoryContextDelete(ctx.memo_cxt);

    return status;
}
