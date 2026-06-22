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
 * SubPlan 检测
 * ======================================================================== */

static bool
pg_cascades_contains_subplan_walker(Node *node, void *context)
{
    if (node == NULL)
        return false;
    if (IsA(node, SubPlan) || IsA(node, AlternativeSubPlan))
        return true;
    return expression_tree_walker(node,
                                  pg_cascades_contains_subplan_walker,
                                  context);
}

static bool
pg_cascades_contains_subplan(Node *node)
{
    return pg_cascades_contains_subplan_walker(node, NULL);
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
    if (pg_cascades_contains_subplan((Node *) parse->targetList) ||
        pg_cascades_contains_subplan((Node *) parse->jointree) ||
        pg_cascades_contains_subplan(parse->havingQual) ||
        pg_cascades_contains_subplan(parse->limitOffset) ||
        pg_cascades_contains_subplan(parse->limitCount))
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
    int         num_rules;
    MemoryContext old_cxt;

    MemSet(&ctx, 0, sizeof(PgPlannerCascadesContext));

    /* 1. Create Cascades MemoryContext */
    ctx.memo_cxt = AllocSetContextCreate(root->planner_cxt,
                                         "PgCascadesMemo",
                                         ALLOCSET_DEFAULT_MINSIZE,
                                         ALLOCSET_DEFAULT_INITSIZE,
                                         ALLOCSET_DEFAULT_MAXSIZE);
    old_cxt = MemoryContextSwitchTo(ctx.memo_cxt);

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

    /* 3. Set up rules */
    {
        int num_impl, num_trans;

        rules = pg_cascades_get_impl_rules(&num_impl);
        ctx.impl_rules = rules;
        ctx.num_impl_rules = num_impl;

        ctx.trans_rules = pg_cascades_get_trans_rules(&num_trans);
        ctx.num_trans_rules = num_trans;
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

    /* 6. Build Memo (logical root + import paths) */
    pg_cascades_build_logical_root(&ctx);

    if (ctx.memo->root_group == NULL)
    {
        MemoryContextSwitchTo(old_cxt);
        MemoryContextDelete(ctx.memo_cxt);
        return PG_CASCADES_INTERNAL_NO_PLAN;
    }

    /* 7. Derive logical property */
    pg_memo_derive_logical_property(ctx.memo, ctx.memo->root_group, &ctx);

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
        debug_print_cascades_memo(&ctx);

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
