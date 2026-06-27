/*-------------------------------------------------------------------------
 * task.c
 *    Cascades Task Scheduler: LIFO stack + 6 种 task 执行函数
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "optimizer/cost.h"
#include "optimizer/clauses.h"
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

        if (task->type == PG_TASK_OPTIMIZE_EXPRESSION ||
            task->type == PG_TASK_ENFORCE_AND_COST ||
            task->type == PG_TASK_APPLY_RULE ||
            task->type == PG_TASK_DERIVE_STATS)
        {
            if (task->expr == NULL)
            {
                elog(WARNING, "Cascades: NULL expr in task type=%d, skipping", task->type);
                pfree(task);
                continue;
            }
        }

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

PgCascadesStatus
pg_task_optimize_group(PgPlannerCascadesContext *ctx, PgOptimizerTask *task)
{
    PgMemoGroup *group = task->group;
    ListCell   *lc;

    /* Already optimized? Skip */
    if (group->best_entries != NIL || group->optimized)
        return PG_CASCADES_OK;

    /*
     * Phase 6: Cost lower-bound pruning (StarRocks parity).
     * If this group's lower bound already exceeds the current global
     * upper bound, no plan using this group can beat the best plan
     * found so far — skip the entire group.
     */
    if (group->lower_bound_cost > 0 &&
        ctx->upper_bound_cost > 0 &&
        group->lower_bound_cost > ctx->upper_bound_cost)
    {
        group->optimized = true;  /* don't retry */
        if (ctx->debug)
            elog(NOTICE, "Cascades: pruned group %d (lower_bound=%.2f >= upper=%.2f)",
                 group->id, group->lower_bound_cost, ctx->upper_bound_cost);
        return PG_CASCADES_OK;
    }

    /* Step 1: push EnforceAndCostTask for physical exprs FIRST.
     * LIFO means they execute last (after children and rule application).
     * Push multi-property tasks for all relevant pathkeys (NIL, sort,
     * group, distinct) to exercise Clone+Resume and multi-property
     * optimization paths. */
    CHECK_FOR_INTERRUPTS();
    foreach(lc, group->physical_exprs)
    {
        PgGroupExpr *expr = (PgGroupExpr *) lfirst(lc);
        pg_cascades_push_enforce_and_cost_tasks(ctx, group, expr);
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
            if (child == group)
                continue;
            if (child->best_entries == NIL)
            {
                PgOptimizerTask *t = palloc0(sizeof(PgOptimizerTask));
                t->type = PG_TASK_OPTIMIZE_GROUP;
                t->group = child;
                task_stack_push(ctx, t);
            }
        }
    }
    group->optimized = true;
    return PG_CASCADES_OK;
}

/* ========================================================================
 * OptimizeExpressionTask
 * ======================================================================== */

PgCascadesStatus
pg_task_optimize_expression(PgPlannerCascadesContext *ctx, PgOptimizerTask *task)
{
    PgGroupExpr *expr = task->expr;
    ListCell   *lc;
    int         i;

    if (expr == NULL)
        return PG_CASCADES_OK;

    /* Step 1: push ApplyRuleTask for each applicable rule */
    /* Implementation rules — high promise first (already sorted) */
    for (i = 0; i < ctx->num_impl_rules; i++)
    {
        PgRule *rule = &ctx->impl_rules[i];
        bool    matches = false;

        /* Phase 5: pattern-based matching */
        if (rule->pattern != NULL)
        {
            /*
             * Fast-path guard: if from_op is set, the root expression's op
             * must match before pattern matching.
             */
            if (rule->from_op != 0 && rule->from_op != expr->op)
            {
                matches = false;
            }
            else
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
        }
        else if (rule->from_op == expr->op)
            matches = true;
        else
            matches = false;

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

    /*
     * Transformation rules are intentionally NOT pushed during the task
     * scheduler optimization phase.  They already ran exhaustively during
     * the rewrite pipeline (pg_cascades_logical_rewrite).
     *
     * Running them here would cause unbounded combinatorial growth: with
     * 31 transformation rules, many matching the same operator kind, each
     * creating new expressions that trigger additional OPTIMIZE_EXPRESSION
     * tasks — even with correct explored_rules tracking and hash table
     * dedup, the task count grows exponentially for non-trivial queries.
     *
     * This separation (rewrite explores the logical plan space; task
     * scheduler optimizes the physical plan space) is the standard
     * Cascades architecture, matching both the Columbia optimizer and
     * StarRocks' two-phase design.
     */

    /* Step 2: push DeriveStatsTask */
    if (!expr->stats_derived)
    {
        PgOptimizerTask *t;

        t = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
        t->type = PG_TASK_DERIVE_STATS;
        t->expr = expr;
        task_stack_push(ctx, t);
    }

    /* Step 3: push ExploreGroupTask for each child.
     * Skip self-referencing children to prevent infinite recursion
     * caused by group merging (e.g., when a LOGICAL_JOIN child group
     * gets merged into its own parent group). */
    foreach(lc, expr->inputs)
    {
        PgMemoGroup *child = (PgMemoGroup *) lfirst(lc);

        if (child == expr->owner_group)
            continue;

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

PgCascadesStatus
pg_task_explore_group(PgPlannerCascadesContext *ctx, PgOptimizerTask *task)
{
    return pg_task_optimize_group(ctx, task);
}

/* ========================================================================
 * DeriveStatsTask
 * ======================================================================== */

PgCascadesStatus
pg_task_derive_stats(PgPlannerCascadesContext *ctx, PgOptimizerTask *task)
{
    PgGroupExpr *expr = task->expr;
    double rows = 0;
    int    width = 0;

    if (expr->stats_derived)
        return PG_CASCADES_OK;

    /* Compute per-expression row count and width estimates */
    pg_derive_expr_stats(ctx, expr, &rows, &width);

    /* Store results on the owning group */
    if (expr->owner_group != NULL)
    {
        if (rows > 0 && expr->owner_group->rows <= 0)
            expr->owner_group->rows = rows;
        if (width > 0 && expr->owner_group->width <= 0)
            expr->owner_group->width = width;

        /* Phase 6: populate structured statistics */
        if (!expr->owner_group->stats.derived)
        {
            expr->owner_group->stats.row_count = rows;
            expr->owner_group->stats.width     = width;
            expr->owner_group->stats.derived   = true;
        }
    }

    expr->stats_derived = true;
    return PG_CASCADES_OK;
}

/* ========================================================================
 * ApplyRuleTask
 * ======================================================================== */

/*
 * push_property_task: helper to create an EnforceAndCostTask for a
 * specific required property (identified by pathkeys), with dedup
 * against previously-pushed properties.
 */
static void
push_property_task(PgPlannerCascadesContext *ctx, PgGroupExpr *expr,
                   List *pathkeys, List **pushed)
{
    ListCell *lc;

    /* Dedup: skip if this pathkeys already pushed */
    foreach(lc, *pushed)
    {
        List *pk = (List *) lfirst(lc);
        if (pk == pathkeys)
            return;
    }

    /* Create new required property and task */
    {
        PgRequiredProperty *req;
        PgOptimizerTask *t;

        req = (PgRequiredProperty *) palloc0(sizeof(PgRequiredProperty));
        req->pathkeys = pathkeys;
        req->required_outer = NULL;
        /* Note: tuple_fraction and limit_tuples stay at 0.0 (palloc0).
         * Setting them to non-zero values would break pg_required_property_equal
         * matching in ENFORCE_OPTIMIZE_CHILDREN when searching for child best
         * entries, because child tasks use palloc0 too. */

        t = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
        t->type = PG_TASK_ENFORCE_AND_COST;
        t->expr = expr;
        t->required = req;
        task_stack_push(ctx, t);

        *pushed = lappend(*pushed, pathkeys);
    }
}

/*
 * pg_cascades_push_enforce_and_cost_tasks:
 *   为新 physical expression 创建 EnforceAndCostTask。
 *
 *   Phase 6: Multi-property optimization.  For each physical expression,
 *   create tasks for all relevant required property combinations:
 *     1. No ordering (NIL pathkeys) — always
 *     2. Sort ordering (sort_pathkeys) — if query has ORDER BY
 *     3. Group ordering (group_pathkeys) — if query has GROUP BY
 *        (for GroupAgg which requires sorted input)
 *     4. Distinct ordering (distinct_pathkeys) — if query has DISTINCT
 *        (for Unique sorted which requires sorted input)
 *
 *   The cost-based search picks the cheapest plan for each
 *   required property; the plan builder selects the one matching
 *   the query's actual requirements.
 */
void
pg_cascades_push_enforce_and_cost_tasks(PgPlannerCascadesContext *ctx,
                                         PgMemoGroup *group,
                                         PgGroupExpr *expr)
{
    List *pushed = NIL;

    (void) group;  /* unused in this function */

    /* 1. No ordering — always needed as baseline */
    push_property_task(ctx, expr, NIL, &pushed);

    /* 2. Sort ordering from ORDER BY clause */
    if (ctx->upper->sort_pathkeys != NIL)
        push_property_task(ctx, expr, ctx->upper->sort_pathkeys, &pushed);

    /* 3. Group ordering from GROUP BY clause (for GroupAgg) */
    if (ctx->upper->group_pathkeys != NIL)
        push_property_task(ctx, expr, ctx->upper->group_pathkeys, &pushed);

    /* 4. Distinct ordering from DISTINCT clause (for Unique sorted) */
    if (ctx->upper->distinct_pathkeys != NIL)
        push_property_task(ctx, expr, ctx->upper->distinct_pathkeys, &pushed);

    list_free(pushed);
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
            int      old_logical_count;

            /* Set binder for this match */
            g_current_binder = binder;
            expr->explored_rules = bms_add_member(expr->explored_rules,
                                                   rule->rule_bit);

            /* Record count before transform (for group merging detection) */
            old_logical_count = list_length(expr->owner_group->logical_exprs);

            /* transform with this binder */
            new_exprs = rule->transform(ctx, expr);

            /*
             * Group merging detection: if transform returned NIL but the
             * group now has more logical expressions, a group-merging
             * rule was applied.  Push tasks for the new expressions.
             */
            if (new_exprs == NIL &&
                list_length(expr->owner_group->logical_exprs) > old_logical_count)
            {
                int new_count = list_length(expr->owner_group->logical_exprs);
                int i;

                for (i = old_logical_count; i < new_count; i++)
                {
                    PgGroupExpr *newe = (PgGroupExpr *)
                        list_nth(expr->owner_group->logical_exprs, i);
                    PgOptimizerTask *t;

                    t = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
                    t->type = PG_TASK_OPTIMIZE_EXPRESSION;
                    t->expr = newe;
                    t->group = expr->owner_group;
                    task_stack_push(ctx, t);
                }

                if (ctx->debug)
                    elog(NOTICE, "Cascades: rule '%s' merged %d expressions into group %d",
                         rule->name, new_count - old_logical_count,
                         expr->owner_group->id);

                continue;  /* skip normal insert loop */
            }

            /* insert into Memo */
            foreach(nlc, new_exprs)
            {
                PgGroupExpr *new_expr = (PgGroupExpr *) lfirst(nlc);
                PgMemoGroup *group;

                CHECK_FOR_INTERRUPTS();

                new_expr->applied_rules = bms_add_member(
                    expr->applied_rules, rule->rule_bit);

                /* Inherit explored_rules to prevent infinite re-application */
                new_expr->explored_rules = bms_copy(expr->explored_rules);

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
        int      old_logical_count;

        if (expr->op != rule->from_op)
            return PG_CASCADES_OK;

        /* Dual BitSet: skip if already explored */
        if (bms_is_member(rule->rule_bit, expr->explored_rules))
            return PG_CASCADES_OK;

        expr->explored_rules = bms_add_member(expr->explored_rules,
                                               rule->rule_bit);

        /* Record logical_exprs count before transform (for group merging detection) */
        old_logical_count = list_length(expr->owner_group->logical_exprs);

        g_current_binder = NULL;
        new_exprs = rule->transform(ctx, expr);
        g_current_binder = NULL;

        /*
         * Phase 5: Group merging detection.
         * If transform returned NIL but the owner group now has more
         * logical expressions than before, a group-merging rule (like
         * EliminateLimit D3 or EliminateAgg F1) has merged child group
         * expressions into this group.  Push OptimizeExpressionTask for
         * each newly added expression.
         */
        if (new_exprs == NIL &&
            list_length(expr->owner_group->logical_exprs) > old_logical_count)
        {
            int new_count = list_length(expr->owner_group->logical_exprs);
            int i;

            for (i = old_logical_count; i < new_count; i++)
            {
                PgGroupExpr *new_expr = (PgGroupExpr *)
                    list_nth(expr->owner_group->logical_exprs, i);
                PgOptimizerTask *t;

                t = (PgOptimizerTask *) palloc0(sizeof(PgOptimizerTask));
                t->type = PG_TASK_OPTIMIZE_EXPRESSION;
                t->expr = new_expr;
                t->group = expr->owner_group;
                task_stack_push(ctx, t);
            }

            if (ctx->debug)
                elog(NOTICE, "Cascades: rule '%s' merged %d expressions into group %d",
                     rule->name, new_count - old_logical_count,
                     expr->owner_group->id);

            return PG_CASCADES_OK;
        }

        foreach(lc, new_exprs)
        {
            PgGroupExpr *new_expr = (PgGroupExpr *) lfirst(lc);
            PgMemoGroup *group;

            CHECK_FOR_INTERRUPTS();

            new_expr->applied_rules = bms_add_member(expr->applied_rules,
                                                       rule->rule_bit);

            /* Phase 6: Inherit explored_rules to prevent re-exploring
             * already-tried rules on derived expressions. */
            if (expr->explored_rules != NULL)
                new_expr->explored_rules = bms_copy(expr->explored_rules);

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

PgOptimizerTask *
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
            path->total_cost > ctx->upper_bound_cost)
            return PG_CASCADES_OK;

        entry = (PgGroupBestEntry *) palloc0(sizeof(PgGroupBestEntry));
        entry->required = pg_required_property_copy(ctx, required);
        entry->expr = expr;
        entry->startup_cost = path->startup_cost;
        entry->total_cost = path->total_cost;
        entry->child_required_props = NIL;
        entry->output = output;

        pg_group_update_best(expr->owner_group, entry);

        /* IMPORTED_PATH: never update global upper bound */

        return PG_CASCADES_OK;
    }

    /* ================================================================
     * COMPOSABLE_OP: scan — skip (IMPORTED_PATH already handles them).
     * COMPOSABLE_OP: join — fast-path costing below (avoids state machine).
     * ================================================================ */
    if (expr->mode == PG_PHYS_EXPR_COMPOSABLE_OP &&
        (expr->op >= PG_CASCADES_PHYSICAL_SEQSCAN &&
         expr->op <= PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN))
        return PG_CASCADES_OK;

    /* COMPOSABLE_OP join: fast-path costing from statistics.
     * IMPORTED_PATH join entries already handle production plan extraction;
     * these entries validate the cost model and serve as Phase 2 foundation. */
    if (expr->mode == PG_PHYS_EXPR_COMPOSABLE_OP &&
        (expr->op >= PG_CASCADES_PHYSICAL_NESTLOOP &&
         expr->op <= PG_CASCADES_PHYSICAL_MERGEJOIN))
    {
        PgOutputProperty output;
        PgGroupBestEntry *entry;
        Cost child_total = 0;
        double outer_rows = 100, inner_rows = 100;
        int    outer_width = 10, inner_width = 10;
        Cost   local_cost;

        MemSet(&output, 0, sizeof(PgOutputProperty));

        /* Gather child stats */
        if (list_length(expr->inputs) >= 2)
        {
            PgMemoGroup *outer_grp = (PgMemoGroup *) linitial(expr->inputs);
            PgMemoGroup *inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

            if (outer_grp->rows > 0) outer_rows = outer_grp->rows;
            if (inner_grp->rows > 0) inner_rows = inner_grp->rows;
            if (outer_grp->width > 0) outer_width = outer_grp->width;
            if (inner_grp->width > 0) inner_width = inner_grp->width;

            /* Accumulate child costs from best entries (use cheapest) */
            if (list_length(outer_grp->best_entries) > 0)
            {
                PgGroupBestEntry *be = (PgGroupBestEntry *)
                    linitial(outer_grp->best_entries);
                child_total += be->total_cost;
            }
            if (list_length(inner_grp->best_entries) > 0)
            {
                PgGroupBestEntry *be = (PgGroupBestEntry *)
                    linitial(inner_grp->best_entries);
                child_total += be->total_cost;
            }
        }

        /* Compute local join cost */
        switch (expr->op)
        {
            case PG_CASCADES_PHYSICAL_NESTLOOP:
                local_cost = outer_rows * inner_rows * cpu_tuple_cost * 0.01;
                break;
            case PG_CASCADES_PHYSICAL_HASHJOIN:
                local_cost = inner_rows * cpu_operator_cost
                           + inner_rows * inner_width * cpu_tuple_cost * 0.01;
                break;
            case PG_CASCADES_PHYSICAL_MERGEJOIN:
                local_cost = outer_rows * cpu_operator_cost * 0.5;
                break;
            default:
                local_cost = 1.0;
                break;
        }
        if (local_cost < 0.01) local_cost = 0.01;

        /* Output properties: conservative — no guaranteed pathkeys */
        output.pathkeys = NIL;
        output.required_outer = NULL;
        output.rows = outer_rows * inner_rows * 0.1;  /* join selectivity */
        output.width = outer_width + inner_width;

        /* Phase 4: upper-bound pruning */
        if (ctx->upper_bound_cost > 0 &&
            child_total + local_cost > ctx->upper_bound_cost * 2.0)
            return PG_CASCADES_OK;

        /* Create best entry with conservative cost (higher than IMPORTED_PATH) */
        entry = (PgGroupBestEntry *) palloc0(sizeof(PgGroupBestEntry));
        entry->required = pg_required_property_copy(ctx, required);
        entry->expr = expr;
        entry->startup_cost = local_cost;
        entry->total_cost = child_total + local_cost + 10000.0; /* penalty */
        entry->child_required_props = NIL;
        entry->output = output;

        pg_group_update_best(expr->owner_group, entry);
        return PG_CASCADES_OK;
    }

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
                    if (child_group->optimized &&
                        list_length(child_group->best_entries) == 1)
                    {
                        child_best = (PgGroupBestEntry *)
                            linitial(child_group->best_entries);
                    }
                    else if (child_group->optimized)
                    {
                        task->cur_child_index++;
                        continue;
                    }

                    /*
                     * Phase 6: Cost lower-bound pruning.
                     * If the child already has a lower bound that exceeds
                     * the global upper bound, this child group has been
                     * proven too expensive — skip.
                     */
                    if (child_group->lower_bound_cost > 0 &&
                        ctx->upper_bound_cost > 0 &&
                        child_group->lower_bound_cost > ctx->upper_bound_cost)
                    {
                        /* Record failure on parent too */
                        if (ctx->upper_bound_cost > 0)
                            expr->owner_group->lower_bound_cost =
                                ctx->upper_bound_cost;
                        task->enforce_state = ENFORCE_COMPLETE;
                        return PG_CASCADES_OK;
                    }

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

            /* If all children were skipped (all optimized with no best entries),
             * don't proceed to cost computation — this expression is dead. */
            if (list_length(expr->inputs) > 0 && task->total_cost == 0)
                return PG_CASCADES_OK;
            task->enforce_state = ENFORCE_COMPUTE_COST;
            /* fall through */
        }

        /* ------------------------------------------------------------
         * ENFORCE_COMPUTE_COST: compute local cost with PG cost model,
         * check pruning + property satisfaction
         * ------------------------------------------------------------ */
        case ENFORCE_COMPUTE_COST:
        {
            Cost child_startup = task->startup_cost;
            Cost child_total   = task->total_cost;
            double input_rows = 0;
            int    input_width = 0;

            /*
             * Phase 6: Per-expression cost caching.
             * If we already have a best cost for this expression (from a
             * previous EnforceAndCostTask with different required property),
             * and the child costs alone exceed it, skip computing local cost.
             */
            if (expr->best_cost > 0 && child_total >= expr->best_cost)
                return PG_CASCADES_OK;

            /*
             * Get child output properties (rows, width) for cost
             * functions that need them.  For expressions without
             * children (e.g. scans), these stay at 0.
             */
            if (list_length(expr->inputs) >= 1)
            {
                PgMemoGroup *child0 = (PgMemoGroup *)
                    linitial(expr->inputs);
                if (list_length(child0->best_entries) > 0)
                {
                    PgGroupBestEntry *cbe = (PgGroupBestEntry *)
                        linitial(child0->best_entries);
                    input_rows = cbe->output.rows;
                    input_width = cbe->output.width;
                }
            }
            if (input_rows <= 0) input_rows = 100;
            if (input_width <= 0) input_width = 10;

            /*
             * Compute local cost using PG cost functions.
             *
             * For most PG cost functions (cost_agg, cost_sort),
             * the result ALREADY includes child costs — the function
             * takes input_total_cost as a parameter and returns the
             * total including both local and input.
             *
             * For Project and Limit, we compute local cost and
             * add it to child costs.
             */
            switch (expr->op)
            {
                case PG_CASCADES_PHYSICAL_HASHAGG:
                {
                    Path dummy_path;
                    MemSet(&dummy_path, 0, sizeof(Path));
                    dummy_path.pathtype = T_Agg;
                    cost_agg(&dummy_path, ctx->root, AGG_HASHED,
                             &ctx->upper->agg_costs,
                             ctx->upper->numGroupCols,
                             ctx->upper->dNumGroups,
                             child_startup, child_total, input_rows);
                    task->startup_cost = dummy_path.startup_cost;
                    task->total_cost = dummy_path.total_cost;
                    break;
                }
                case PG_CASCADES_PHYSICAL_GROUPAGG:
                {
                    Path dummy_path;
                    MemSet(&dummy_path, 0, sizeof(Path));
                    dummy_path.pathtype = T_Agg;
                    cost_agg(&dummy_path, ctx->root, AGG_SORTED,
                             &ctx->upper->agg_costs,
                             ctx->upper->numGroupCols,
                             ctx->upper->dNumGroups,
                             child_startup, child_total, input_rows);
                    task->startup_cost = dummy_path.startup_cost;
                    task->total_cost = dummy_path.total_cost;
                    break;
                }
                case PG_CASCADES_PHYSICAL_SORT:
                {
                    Path dummy_path;
                    double limit_tuples = required->limit_tuples;
                    MemSet(&dummy_path, 0, sizeof(Path));
                    dummy_path.pathtype = T_Sort;
                    cost_sort(&dummy_path, ctx->root,
                              required->pathkeys,
                              child_total,
                              input_rows, input_width,
                              0.0,         /* comparison_cost */
                              work_mem,
                              (limit_tuples > 0) ? limit_tuples : -1.0);
                    task->startup_cost = dummy_path.startup_cost;
                    task->total_cost = dummy_path.total_cost;
                    break;
                }
                case PG_CASCADES_PHYSICAL_UNIQUE:
                {
                    Path dummy_path;
                    MemSet(&dummy_path, 0, sizeof(Path));
                    dummy_path.pathtype = T_Unique;
                    cost_sort(&dummy_path, ctx->root,
                              ctx->upper->distinct_pathkeys,
                              child_total,
                              input_rows, input_width,
                              0.0, work_mem, -1.0);
                    task->startup_cost = dummy_path.startup_cost;
                    task->total_cost = dummy_path.total_cost;
                    break;
                }
                case PG_CASCADES_PHYSICAL_LIMIT:
                {
                    double frac = 1.0;
                    if (required->limit_tuples > 0 &&
                        required->limit_tuples < input_rows)
                        frac = required->limit_tuples / input_rows;
                    task->startup_cost = child_startup;
                    task->total_cost = child_startup +
                        (child_total - child_startup) * frac;
                    break;
                }
                case PG_CASCADES_PHYSICAL_PROJECT:
                {
                    QualCost qcost;
                    MemSet(&qcost, 0, sizeof(QualCost));
                    cost_qual_eval(&qcost, ctx->upper->tlist, ctx->root);
                    task->startup_cost = child_startup + qcost.startup;
                    task->total_cost = child_total +
                        qcost.per_tuple * input_rows;
                    break;
                }
                default:
                    task->startup_cost = child_startup + 0.01;
                    task->total_cost = child_total + 0.01;
                    break;
            }
            (void) 0;

            /* Per-expression pruning (above) via expr->best_cost
             * already prevents redundant ENFORCE_AND_COST runs. */

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

                /*
                 * Phase 6: Per-expression cost caching.
                 * Cache the best cost for this expression to enable
                 * early pruning in subsequent EnforceAndCostTask runs
                 * (for different required properties).
                 */
                if (expr->best_cost == 0 ||
                    task->total_cost < expr->best_cost)
                    expr->best_cost = task->total_cost;

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
