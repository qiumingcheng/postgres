/*-------------------------------------------------------------------------
 * task.c
 *    Cascades Task Scheduler: LIFO stack + 6 种 task 执行函数
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

/* ========================================================================
 * Task Stack 操作（LIFO）
 * ======================================================================== */

bool
task_stack_empty(PgPlannerCascadesContext *ctx)
{
    return ctx->task_stack == NIL;
}

void
task_stack_push(PgPlannerCascadesContext *ctx, PgOptimizerTask *task)
{
    ctx->task_stack = lcons(task, ctx->task_stack);
}

PgOptimizerTask *
task_stack_pop(PgPlannerCascadesContext *ctx)
{
    PgOptimizerTask *task;

    if (ctx->task_stack == NIL)
        return NULL;

    task = (PgOptimizerTask *) linitial(ctx->task_stack);
    ctx->task_stack = list_delete_first(ctx->task_stack);
    return task;
}

/* ========================================================================
 * Limit Check
 * ======================================================================== */

PgCascadesStatus
pg_cascades_check_limits(PgPlannerCascadesContext *ctx)
{
    ctx->num_tasks_executed++;

    if (ctx->max_groups > 0 &&
        list_length(ctx->memo->groups) > ctx->max_groups)
        return PG_CASCADES_INTERNAL_LIMIT;

    if (ctx->max_tasks > 0 &&
        ctx->num_tasks_executed > ctx->max_tasks)
        return PG_CASCADES_INTERNAL_LIMIT;

    if (ctx->timeout_ms > 0 &&
        TimestampDifferenceExceeds(ctx->start_time, GetCurrentTimestamp(),
                                   ctx->timeout_ms))
        return PG_CASCADES_INTERNAL_TIMEOUT;

    return PG_CASCADES_OK;
}

/* ========================================================================
 * Forward declarations of task executors
 * ======================================================================== */

static PgCascadesStatus pg_task_optimize_group(PgPlannerCascadesContext *ctx,
                                                PgOptimizerTask *task);
static PgCascadesStatus pg_task_optimize_expression(PgPlannerCascadesContext *ctx,
                                                     PgOptimizerTask *task);
static PgCascadesStatus pg_task_explore_group(PgPlannerCascadesContext *ctx,
                                               PgOptimizerTask *task);
static PgCascadesStatus pg_task_derive_stats(PgPlannerCascadesContext *ctx,
                                              PgOptimizerTask *task);
static PgCascadesStatus pg_task_apply_rule(PgPlannerCascadesContext *ctx,
                                            PgOptimizerTask *task);
static PgCascadesStatus pg_task_enforce_and_cost(PgPlannerCascadesContext *ctx,
                                                  PgOptimizerTask *task);

/* ========================================================================
 * 主循环
 * ======================================================================== */

PgCascadesStatus
pg_cascades_run_tasks(PgPlannerCascadesContext *ctx)
{
    while (!task_stack_empty(ctx))
    {
        PgOptimizerTask *task = task_stack_pop(ctx);
        PgCascadesStatus status;

        CHECK_FOR_INTERRUPTS();

        status = pg_cascades_check_limits(ctx);
        if (status != PG_CASCADES_OK)
            return status;

        switch (task->type)
        {
            case PG_TASK_OPTIMIZE_GROUP:
                status = pg_task_optimize_group(ctx, task);
                break;
            case PG_TASK_OPTIMIZE_EXPRESSION:
                status = pg_task_optimize_expression(ctx, task);
                break;
            case PG_TASK_EXPLORE_GROUP:
                status = pg_task_explore_group(ctx, task);
                break;
            case PG_TASK_DERIVE_STATS:
                status = pg_task_derive_stats(ctx, task);
                break;
            case PG_TASK_APPLY_RULE:
                status = pg_task_apply_rule(ctx, task);
                break;
            case PG_TASK_ENFORCE_AND_COST:
                status = pg_task_enforce_and_cost(ctx, task);
                break;
            default:
                elog(ERROR, "unknown task type: %d", task->type);
                status = PG_CASCADES_UNSUPPORTED;
                break;
        }

        pfree(task);

        if (status != PG_CASCADES_OK)
            return status;
    }

    return PG_CASCADES_OK;
}

/* ========================================================================
 * OptimizeGroupTask
 * ======================================================================== */

static PgCascadesStatus
pg_task_optimize_group(PgPlannerCascadesContext *ctx, PgOptimizerTask *task)
{
    PgMemoGroup *group = task->group;
    ListCell   *lc;

    if (group->best_entries != NIL)
        return PG_CASCADES_OK;  /* already optimized */

    /* Step 1: push EnforceAndCostTask for physical exprs FIRST.
     * LIFO means they execute last (after children and rule application). */
    CHECK_FOR_INTERRUPTS();
    foreach(lc, group->physical_exprs)
    {
        PgGroupExpr *expr = (PgGroupExpr *) lfirst(lc);
        PgOptimizerTask *t;

        t = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
        t->type = PG_TASK_ENFORCE_AND_COST;
        t->expr = expr;
        t->required = NULL;
        task_stack_push(ctx, t);
    }

    /* Step 2: push OptimizeExpressionTask for each logical expr */
    foreach(lc, group->logical_exprs)
    {
        PgGroupExpr *expr = (PgGroupExpr *) lfirst(lc);
        PgOptimizerTask *t;

        t = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
        t->type = PG_TASK_OPTIMIZE_EXPRESSION;
        t->group = group;
        t->expr = expr;
        task_stack_push(ctx, t);
    }

    /* Step 3: push child OptimizeGroupTasks LAST.
     * LIFO means children execute FIRST (bottom-up optimization). */
    foreach(lc, group->logical_exprs)
    {
        PgGroupExpr *expr = (PgGroupExpr *) lfirst(lc);
        ListCell   *cl;
        foreach(cl, expr->inputs)
        {
            PgMemoGroup *child = (PgMemoGroup *) lfirst(cl);
            if (child->best_entries == NIL)
            {
                PgOptimizerTask *t = palloc0(sizeof(PgOptimizerTask));
                t->type = PG_TASK_OPTIMIZE_GROUP;
                t->group = child;
                task_stack_push(ctx, t);
            }
        }
    }

    return PG_CASCADES_OK;
}

/* ========================================================================
 * OptimizeExpressionTask
 * ======================================================================== */

static PgCascadesStatus
pg_task_optimize_expression(PgPlannerCascadesContext *ctx, PgOptimizerTask *task)
{
    PgGroupExpr *expr = task->expr;
    ListCell   *lc;
    int         i;

    /* Step 1: push ApplyRuleTask for each applicable rule */
    /* Implementation rules */
    for (i = 0; i < ctx->num_impl_rules; i++)
    {
        if (ctx->impl_rules[i].from_op == expr->op)
        {
            PgOptimizerTask *t;

            t = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
            t->type = PG_TASK_APPLY_RULE;
            t->expr = expr;
            t->rule = &ctx->impl_rules[i];
            task_stack_push(ctx, t);
        }
    }

    /* Transformation rules (Phase 3) */
    for (i = 0; i < ctx->num_trans_rules; i++)
    {
        if (ctx->trans_rules[i].from_op == expr->op)
        {
            PgOptimizerTask *t;

            t = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
            t->type = PG_TASK_APPLY_RULE;
            t->expr = expr;
            t->rule = &ctx->trans_rules[i];
            task_stack_push(ctx, t);
        }
    }

    /* Step 2: push DeriveStatsTask */
    if (!expr->stats_derived)
    {
        PgOptimizerTask *t;

        t = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
        t->type = PG_TASK_DERIVE_STATS;
        t->expr = expr;
        task_stack_push(ctx, t);
    }

    /* Step 3: push ExploreGroupTask for each child */
    foreach(lc, expr->inputs)
    {
        PgMemoGroup *child = (PgMemoGroup *) lfirst(lc);
        PgOptimizerTask *t;

        t = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
        t->type = PG_TASK_EXPLORE_GROUP;
        t->group = child;
        task_stack_push(ctx, t);
    }

    return PG_CASCADES_OK;
}

/* ========================================================================
 * ExploreGroupTask
 * ======================================================================== */

static PgCascadesStatus
pg_task_explore_group(PgPlannerCascadesContext *ctx, PgOptimizerTask *task)
{
    return pg_task_optimize_group(ctx, task);
}

/* ========================================================================
 * DeriveStatsTask
 * ======================================================================== */

static PgCascadesStatus
pg_task_derive_stats(PgPlannerCascadesContext *ctx, PgOptimizerTask *task)
{
    PgGroupExpr *expr = task->expr;

    if (expr->stats_derived)
        return PG_CASCADES_OK;

    expr->stats_derived = true;
    return PG_CASCADES_OK;
}

/* ========================================================================
 * ApplyRuleTask
 * ======================================================================== */

/*
 * pg_cascades_push_enforce_and_cost_tasks:
 *   为新 physical expression 创建 EnforceAndCostTask。
 */
void
pg_cascades_push_enforce_and_cost_tasks(PgPlannerCascadesContext *ctx,
                                         PgMemoGroup *group,
                                         PgGroupExpr *expr)
{
    PgRequiredProperty *req1;
    PgRequiredProperty *req2;
    PgOptimizerTask *t;

    /* required = NIL pathkeys */
    req1 = (PgRequiredProperty *) palloc0(sizeof(PgRequiredProperty));
    req1->pathkeys = NIL;
    req1->required_outer = NULL;

    t = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
    t->type = PG_TASK_ENFORCE_AND_COST;
    t->expr = expr;
    t->required = req1;
    task_stack_push(ctx, t);

    /* required = root sort_pathkeys (if any) */
    if (ctx->upper->sort_pathkeys != NIL)
    {
        req2 = (PgRequiredProperty *) palloc0(sizeof(PgRequiredProperty));
        req2->pathkeys = ctx->upper->sort_pathkeys;
        req2->required_outer = NULL;

        t = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
        t->type = PG_TASK_ENFORCE_AND_COST;
        t->expr = expr;
        t->required = req2;
        task_stack_push(ctx, t);
    }
}

static PgCascadesStatus
pg_task_apply_rule(PgPlannerCascadesContext *ctx, PgOptimizerTask *task)
{
    PgRule     *rule = (PgRule *) task->rule;
    PgGroupExpr *expr = task->expr;
    List       *new_exprs;
    ListCell   *lc;

    /* check from_op match */
    if (expr->op != rule->from_op)
        return PG_CASCADES_OK;

    /* Dual BitSet: skip if this rule already explored on this expression */
    if (bms_is_member(rule->rule_bit, expr->explored_rules))
        return PG_CASCADES_OK;

    /* mark rule as explored before transform */
    expr->explored_rules = bms_add_member(expr->explored_rules, rule->rule_bit);

    /* transform */
    new_exprs = rule->transform(ctx, expr);

    /* insert into Memo */
    foreach(lc, new_exprs)
    {
        PgGroupExpr *new_expr = (PgGroupExpr *) lfirst(lc);
        PgMemoGroup *group;

        CHECK_FOR_INTERRUPTS();

        /* lineage: new_expr inherits parent's applied_rules + this rule */
        new_expr->applied_rules = bms_add_member(expr->applied_rules,
                                                   rule->rule_bit);

        group = pg_memo_insert_expression(ctx, ctx->memo, new_expr,
                                           expr->owner_group);

        if (group == NULL)
            continue;  /* duplicate — skip */

        if (rule->is_implementation)
            pg_cascades_push_enforce_and_cost_tasks(ctx, group, new_expr);
        else
        {
            /* new logical expr → push OptimizeExpressionTask */
            PgOptimizerTask *t;

            t = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
            t->type = PG_TASK_OPTIMIZE_EXPRESSION;
            t->expr = new_expr;
            t->group = group;
            task_stack_push(ctx, t);
        }
    }

    return PG_CASCADES_OK;
}

/* ========================================================================
 * EnforceAndCostTask
 * ======================================================================== */

static PgCascadesStatus
pg_task_enforce_and_cost(PgPlannerCascadesContext *ctx, PgOptimizerTask *task)
{
    PgGroupExpr *expr = task->expr;
    PgRequiredProperty *required = task->required;
    PgOutputProperty output;
    PgGroupBestEntry *entry;

    CHECK_FOR_INTERRUPTS();

    /* Default required if NULL: no pathkeys */
    if (required == NULL)
    {
        required = (PgRequiredProperty *) palloc0(sizeof(PgRequiredProperty));
        required->pathkeys = NIL;
        required->required_outer = NULL;
    }

    /* IMPORTED_PATH mode: cost from Path directly */
    if (expr->mode == PG_PHYS_EXPR_IMPORTED_PATH)
    {
        Path *path = (Path *) expr->op_private;

        output.pathkeys = path->pathkeys;
        output.required_outer = (path->param_info != NULL) ?
            path->param_info->ppi_req_outer : NULL;
        output.rows = path->parent->rows;
        output.width = path->parent->width;

        if (!pg_output_satisfies_required(&output, required))
            return PG_CASCADES_OK;

        /* Phase 4: upper-bound pruning */
        if (ctx->upper_bound_cost > 0 &&
            path->total_cost >= ctx->upper_bound_cost)
            return PG_CASCADES_OK;

        entry = (PgGroupBestEntry *) palloc0(sizeof(PgGroupBestEntry));
        entry->required = pg_required_property_copy(ctx, required);
        entry->expr = expr;
        entry->startup_cost = path->startup_cost;
        entry->total_cost = path->total_cost;
        entry->child_required_props = NIL;
        entry->output = output;

        pg_group_update_best(expr->owner_group, entry);
        return PG_CASCADES_OK;
    }

    /* COMPOSABLE_OP mode: derive child properties, need recursive cost */
    {
        List *child_required_props;
        PgGroupBestEntry *child_best;
        Cost total_cost = 0;
        Cost startup_cost = 0;
        ListCell *lc;

        pg_derive_child_properties(ctx, expr, required,
                                    &child_required_props, &output);

        /* Add small base cost so competing implementations are differentiated */
        switch (expr->op)
        {
            case PG_CASCADES_PHYSICAL_HASHAGG:
                startup_cost = 0.01; total_cost = 0.01; break;
            case PG_CASCADES_PHYSICAL_GROUPAGG:
                startup_cost = 0.02; total_cost = 0.02; break;
            case PG_CASCADES_PHYSICAL_SORT:
                startup_cost = 1.0; total_cost = 1.0; break;
            default:
                startup_cost = 0.1; total_cost = 0.1; break;
        }

        /* For each child, find best under required property */
        /* First version: only handle single-child upper ops */
        if (list_length(expr->inputs) == 1 &&
            list_length(child_required_props) == 1)
        {
            PgMemoGroup *child_group = (PgMemoGroup *) linitial(expr->inputs);
            PgRequiredProperty *child_req = (PgRequiredProperty *)
                linitial(child_required_props);

            /* Find child best */
            child_best = NULL;
            foreach(lc, child_group->best_entries)
            {
                PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
                if (pg_required_property_equal(e->required, child_req))
                {
                    child_best = e;
                    break;
                }
            }

            if (child_best == NULL)
            {
                /* No matching child best for this required property.
                 * For COMPOSABLE_OP, this means we can't satisfy this
                 * required property via this expression. Skip gracefully. */
                return PG_CASCADES_OK;
            }

            /* Add child cost on top of base cost */
            startup_cost += child_best->startup_cost;
            total_cost += child_best->total_cost;
        }

        /* Phase 4: upper-bound pruning */
        if (ctx->upper_bound_cost > 0 &&
            total_cost >= ctx->upper_bound_cost)
            return PG_CASCADES_OK;

        entry = (PgGroupBestEntry *) palloc0(sizeof(PgGroupBestEntry));
        entry->required = pg_required_property_copy(ctx, required);
        entry->expr = expr;
        entry->startup_cost = startup_cost;
        entry->total_cost = total_cost;
        entry->child_required_props = child_required_props;
        entry->output = output;

        pg_group_update_best(expr->owner_group, entry);
    }

    return PG_CASCADES_OK;
}
