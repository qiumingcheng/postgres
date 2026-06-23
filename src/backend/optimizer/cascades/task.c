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
    /* Implementation rules — high promise first (already sorted) */
    for (i = 0; i < ctx->num_impl_rules; i++)
    {
        PgRule *rule = &ctx->impl_rules[i];
        bool    matches = false;

        /* Phase 5: pattern-based matching */
        if (rule->pattern != NULL)
        {
            ListCell *elc;
            matches = false;
            foreach(elc, expr->owner_group->logical_exprs)
            {
                PgGroupExpr *cand = (PgGroupExpr *) lfirst(elc);
                if (pg_pattern_match_root_only(rule->pattern, cand) != NIL)
                {
                    matches = true;
                    break;
                }
            }
        }
        else if (rule->from_op == expr->op)
            matches = true;

        if (matches)
        {
            PgOptimizerTask *t;

            t = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
            t->type = PG_TASK_APPLY_RULE;
            t->expr = expr;
            t->rule = rule;
            task_stack_push(ctx, t);
        }
    }

    /* Transformation rules — high promise first (already sorted) */
    for (i = 0; i < ctx->num_trans_rules; i++)
    {
        PgRule *rule = &ctx->trans_rules[i];
        bool    matches = false;

        /* Phase 5: pattern-based matching */
        if (rule->pattern != NULL)
        {
            ListCell *elc;
            matches = false;
            foreach(elc, expr->owner_group->logical_exprs)
            {
                PgGroupExpr *cand = (PgGroupExpr *) lfirst(elc);
                if (pg_pattern_match_root_only(rule->pattern, cand) != NIL)
                {
                    matches = true;
                    break;
                }
            }
        }
        else if (rule->from_op == expr->op)
            matches = true;

        if (matches)
        {
            PgOptimizerTask *t;

            t = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
            t->type = PG_TASK_APPLY_RULE;
            t->expr = expr;
            t->rule = rule;
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

/* ========================================================================
 * Phase 5: Binder helper for pattern-based rules
 * ======================================================================== */

static PgBinder *g_current_binder = NULL;

PgBinder *
pg_cascades_get_current_binder(PgPlannerCascadesContext *ctx)
{
    (void) ctx;
    return g_current_binder;
}

/* ========================================================================
 * ApplyRuleTask
 *
 * Phase 5: supports both from_op matching (legacy) and pattern-based
 * matching. When rule->pattern != NULL, pg_pattern_bind is called first
 * and the result is stored in task->binder for the transform function
 * to access via pg_cascades_get_current_binder().
 * ======================================================================== */

static PgCascadesStatus
pg_task_apply_rule(PgPlannerCascadesContext *ctx, PgOptimizerTask *task)
{
    PgRule     *rule = (PgRule *) task->rule;
    PgGroupExpr *expr = task->expr;

    /* Phase 5: pattern-based matching — enumerate all matches */
    if (rule->pattern != NULL)
    {
        ListCell *elc;
        List *all_binders = NIL;

        /* Try matching pattern against each logical expression in the group */
        foreach(elc, expr->owner_group->logical_exprs)
        {
            PgGroupExpr *candidate = (PgGroupExpr *) lfirst(elc);
            List *binders = pg_pattern_match_full(rule->pattern, candidate);
            if (binders != NIL)
                all_binders = list_concat(all_binders, binders);
        }
        if (all_binders == NIL)
            return PG_CASCADES_OK;  /* pattern doesn't match */

        /* Process each match */
        foreach(elc, all_binders)
        {
            PgBinder *binder = (PgBinder *) lfirst(elc);
            List *new_exprs;
            ListCell *nlc;

            /* Set binder for this match */
            g_current_binder = binder;
            expr->explored_rules = bms_add_member(expr->explored_rules,
                                                   rule->rule_bit);

            /* transform with this binder */
            new_exprs = rule->transform(ctx, expr);

            /* insert into Memo */
            foreach(nlc, new_exprs)
            {
                PgGroupExpr *new_expr = (PgGroupExpr *) lfirst(nlc);
                PgMemoGroup *group;

                CHECK_FOR_INTERRUPTS();

                new_expr->applied_rules = bms_add_member(
                    expr->applied_rules, rule->rule_bit);

                group = pg_memo_insert_expression(ctx, ctx->memo,
                                                   new_expr, expr->owner_group);
                if (group == NULL)
                    continue;

                if (rule->rule_type == PG_RULE_IMPL)
                    pg_cascades_push_enforce_and_cost_tasks(ctx, group, new_expr);
                else
                {
                    PgOptimizerTask *t;
                    t = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
                    t->type = PG_TASK_OPTIMIZE_EXPRESSION;
                    t->expr = new_expr;
                    t->group = group;
                    task_stack_push(ctx, t);
                }
            }
        }

        g_current_binder = NULL;
        return PG_CASCADES_OK;
    }

    /* Legacy from_op matching (rules without pattern) */
    {
        List *new_exprs;
        ListCell *lc;

        if (expr->op != rule->from_op)
            return PG_CASCADES_OK;

        /* Dual BitSet: skip if already explored */
        if (bms_is_member(rule->rule_bit, expr->explored_rules))
            return PG_CASCADES_OK;

        expr->explored_rules = bms_add_member(expr->explored_rules,
                                               rule->rule_bit);

        g_current_binder = NULL;
        new_exprs = rule->transform(ctx, expr);
        g_current_binder = NULL;

        foreach(lc, new_exprs)
        {
            PgGroupExpr *new_expr = (PgGroupExpr *) lfirst(lc);
            PgMemoGroup *group;

            CHECK_FOR_INTERRUPTS();

            new_expr->applied_rules = bms_add_member(expr->applied_rules,
                                                       rule->rule_bit);

            group = pg_memo_insert_expression(ctx, ctx->memo, new_expr,
                                               expr->owner_group);
            if (group == NULL)
                continue;

            if (rule->rule_type == PG_RULE_IMPL)
                pg_cascades_push_enforce_and_cost_tasks(ctx, group, new_expr);
            else
            {
                PgOptimizerTask *t;
                t = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
                t->type = PG_TASK_OPTIMIZE_EXPRESSION;
                t->expr = new_expr;
                t->group = group;
                task_stack_push(ctx, t);
            }
        }
    }

    return PG_CASCADES_OK;
}

/* ========================================================================
 * Phase 4: Clone helper for EnforceAndCostTask resume
 * ======================================================================== */

static PgOptimizerTask *
pg_task_clone(PgOptimizerTask *src)
{
    PgOptimizerTask *dst = (PgOptimizerTask *) palloc(sizeof(PgOptimizerTask));
    memcpy(dst, src, sizeof(PgOptimizerTask));
    return dst;
}

/* ========================================================================
 * EnforceAndCostTask — Phase 4 state machine with Clone+Resume
 *
 * States:
 *   ENFORCE_INIT              — derive child properties, set up iteration
 *   ENFORCE_OPTIMIZE_CHILDREN — optimize children one by one (clone+resume)
 *   ENFORCE_COMPUTE_COST      — accumulate costs, check pruning, check
 *                                property satisfaction
 *   ENFORCE_ENFORCE_PROPERTY  — apply Sort enforcer when output mismatches
 *   ENFORCE_COMPLETE          — done
 * ======================================================================== */

static PgCascadesStatus
pg_task_enforce_and_cost(PgPlannerCascadesContext *ctx, PgOptimizerTask *task)
{
    PgGroupExpr *expr = task->expr;
    PgRequiredProperty *required = task->required;

    CHECK_FOR_INTERRUPTS();

    /* Default required if NULL */
    if (required == NULL)
    {
        required = (PgRequiredProperty *) palloc0(sizeof(PgRequiredProperty));
        required->pathkeys = NIL;
        required->required_outer = NULL;
        task->required = required;
    }

    /* ================================================================
     * IMPORTED_PATH: fast path — no children, cost from Path directly
     * ================================================================ */
    if (expr->mode == PG_PHYS_EXPR_IMPORTED_PATH)
    {
        Path *path = (Path *) expr->op_private;
        PgOutputProperty output;
        PgGroupBestEntry *entry;

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

        /* Update global upper bound */
        if (ctx->upper_bound_cost == 0 ||
            path->total_cost < ctx->upper_bound_cost)
            ctx->upper_bound_cost = path->total_cost;

        return PG_CASCADES_OK;
    }

    /* ================================================================
     * COMPOSABLE_OP: state-machine driven
     * ================================================================ */

    switch (task->enforce_state)
    {
        /* ------------------------------------------------------------
         * ENFORCE_INIT: derive child required properties + output
         * ------------------------------------------------------------ */
        case ENFORCE_INIT:
        {
            pg_derive_child_properties(ctx, expr, required,
                                        &task->child_required_props,
                                        &task->output_property);
            task->cur_child_index = 0;
            task->total_cost = 0;
            task->startup_cost = 0;

            /* Add local/base cost */
            switch (expr->op)
            {
                case PG_CASCADES_PHYSICAL_HASHAGG:
                    task->startup_cost = 0.01; task->total_cost = 0.01; break;
                case PG_CASCADES_PHYSICAL_GROUPAGG:
                    task->startup_cost = 0.02; task->total_cost = 0.02; break;
                case PG_CASCADES_PHYSICAL_SORT:
                    task->startup_cost = 1.0; task->total_cost = 1.0; break;
                default:
                    task->startup_cost = 0.1; task->total_cost = 0.1; break;
            }

            task->enforce_state = ENFORCE_OPTIMIZE_CHILDREN;
            /* fall through */
        }

        /* ------------------------------------------------------------
         * ENFORCE_OPTIMIZE_CHILDREN: process children one by one
         * Clone+Resume: if child not ready, push clone + child task
         * ------------------------------------------------------------ */
        case ENFORCE_OPTIMIZE_CHILDREN:
        {
            int n_children = list_length(expr->inputs);
            int n_reqs = list_length(task->child_required_props);

            while (task->cur_child_index < n_children &&
                   task->cur_child_index < n_reqs)
            {
                PgMemoGroup *child_group;
                PgRequiredProperty *child_req;
                PgGroupBestEntry *child_best = NULL;
                ListCell *lc;

                child_group = (PgMemoGroup *)
                    list_nth(expr->inputs, task->cur_child_index);
                child_req = (PgRequiredProperty *)
                    list_nth(task->child_required_props,
                             task->cur_child_index);

                /* Search for matching best entry in child group */
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
                    /*
                     * Child not ready for this required property.
                     * Clone self to resume after child is optimized,
                     * then push OptimizeGroupTask for child.
                     */
                    PgOptimizerTask *clone = pg_task_clone(task);
                    PgOptimizerTask *child_task;

                    clone->cur_child_index++; /* next time, try next */
                    task_stack_push(ctx, clone);

                    child_task = (PgOptimizerTask *)
                        palloc0(sizeof(PgOptimizerTask));
                    child_task->type = PG_TASK_OPTIMIZE_GROUP;
                    child_task->group = child_group;
                    task_stack_push(ctx, child_task);

                    return PG_CASCADES_OK; /* pause */
                }

                /* Child ready: accumulate costs */
                task->startup_cost += child_best->startup_cost;
                task->total_cost += child_best->total_cost;
                task->cur_child_index++;
            }

            task->enforce_state = ENFORCE_COMPUTE_COST;
            /* fall through */
        }

        /* ------------------------------------------------------------
         * ENFORCE_COMPUTE_COST: check pruning + property satisfaction
         * ------------------------------------------------------------ */
        case ENFORCE_COMPUTE_COST:
        {
            /* Phase 4: upper-bound pruning */
            if (ctx->upper_bound_cost > 0 &&
                task->total_cost >= ctx->upper_bound_cost)
                return PG_CASCADES_OK;

            /* Check if output satisfies required property */
            if (!pg_output_satisfies_required(&task->output_property,
                                              required))
            {
                /*
                 * Property mismatch (e.g. required has pathkeys but
                 * child output doesn't). Try enforcer.
                 */
                task->enforce_state = ENFORCE_ENFORCE_PROPERTY;
                /* fall through */
            }
            else
            {
                /* Create best entry */
                PgGroupBestEntry *entry;

                entry = (PgGroupBestEntry *)
                    palloc0(sizeof(PgGroupBestEntry));
                entry->required = pg_required_property_copy(ctx, required);
                entry->expr = expr;
                entry->startup_cost = task->startup_cost;
                entry->total_cost = task->total_cost;
                entry->child_required_props = task->child_required_props;
                entry->output = task->output_property;

                pg_group_update_best(expr->owner_group, entry);

                /* Update global upper bound */
                if (ctx->upper_bound_cost == 0 ||
                    task->total_cost < ctx->upper_bound_cost)
                    ctx->upper_bound_cost = task->total_cost;

                return PG_CASCADES_OK;
            }
        }

        /* ------------------------------------------------------------
         * ENFORCE_ENFORCE_PROPERTY: apply Sort enforcer rule
         * ------------------------------------------------------------ */
        case ENFORCE_ENFORCE_PROPERTY:
        {
            /*
             * Phase 4: Enforcer Task.
             *
             * When required property has pathkeys but child output
             * doesn't satisfy them, insert a PhysicalSort on top
             * of the child group.
             *
             * Phase 5 fix: dedup check — don't create duplicate Sort
             * enforcers for the same (child_group, pathkeys) pair.
             * Also skip creating Sort on top of Sort (Sort(Sort) is
             * redundant).
             */
            if (list_length(expr->inputs) == 1 &&
                required->pathkeys != NIL &&
                expr->op != PG_CASCADES_PHYSICAL_SORT)  /* skip Sort-on-Sort */
            {
                PgMemoGroup *child_group;
                PgGroupExpr *sort_expr;
                PgOptimizerTask *enforcer_task;
                ListCell   *lc;
                bool        already_exists = false;

                child_group = (PgMemoGroup *) linitial(expr->inputs);

                /* Phase 5: dedup — check if Sort enforcer already exists */
                foreach(lc, child_group->physical_exprs)
                {
                    PgGroupExpr *pex = (PgGroupExpr *) lfirst(lc);
                    PgSortPrivate *sp;

                    if (pex->op != PG_CASCADES_PHYSICAL_SORT)
                        continue;
                    if (list_length(pex->inputs) != 1)
                        continue;
                    /* Same child? */
                    if (linitial(pex->inputs) != linitial(expr->inputs))
                        continue;
                    /* Check pathkeys via op_private */
                    sp = (PgSortPrivate *) pex->op_private;
                    if (sp != NULL && sp->pathkeys == required->pathkeys)
                    {
                        already_exists = true;
                        break;
                    }
                }

                if (already_exists)
                {
                    /* Sort enforcer already exists for this (group, pathkeys) */
                    task->enforce_state = ENFORCE_COMPLETE;
                    return PG_CASCADES_OK;
                }

                /* Build PhysicalSort on child group */
                sort_expr = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_PHYSICAL_SORT);
                sort_expr->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
                sort_expr->inputs = list_make1(child_group);

                /* Store pathkeys in PgSortPrivate */
                {
                    PgSortPrivate *sp = (PgSortPrivate *)
                        palloc(sizeof(PgSortPrivate));
                    sp->pathkeys = required->pathkeys;
                    sp->limit_tuples = required->limit_tuples;
                    sort_expr->op_private = sp;
                }

                /* Insert into child group */
                pg_memo_add_physical_expr(child_group, sort_expr);

                /* Clone self to resume after enforcer is costed */
                {
                    PgOptimizerTask *clone = pg_task_clone(task);
                    clone->enforce_state = ENFORCE_COMPUTE_COST;
                    /* Reset: will re-derive after enforcer is ready */
                    clone->child_required_props = NIL;
                    task_stack_push(ctx, clone);
                }

                /* Push EnforceAndCostTask for the Sort enforcer */
                enforcer_task = (PgOptimizerTask *)
                    palloc0(sizeof(PgOptimizerTask));
                enforcer_task->type = PG_TASK_ENFORCE_AND_COST;
                enforcer_task->expr = sort_expr;
                enforcer_task->required =
                    pg_required_property_copy(ctx, required);
                task_stack_push(ctx, enforcer_task);

                return PG_CASCADES_OK; /* pause, enforcer runs first */
            }

            /* Can't enforce — skip */
            return PG_CASCADES_OK;
        }

        /* ------------------------------------------------------------
         * ENFORCE_COMPLETE: no-op
         * ------------------------------------------------------------ */
        case ENFORCE_COMPLETE:
        default:
            return PG_CASCADES_OK;
    }

    return PG_CASCADES_OK;
}
