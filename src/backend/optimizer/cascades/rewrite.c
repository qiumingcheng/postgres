/*-------------------------------------------------------------------------
 * rewrite.c
 *    Cascades Rewrite Phase Pipeline (Phase 4a)
 *    Pre-Memo rule-based logical rewriting in stages.
 *
 *    Now wired up with Phase 5 transformation rules.
 *    Each stage applies a group of rules to the expression tree
 *    before cost-based Memo search begins.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"

/* ========================================================================
 * Forward declarations of rule transform functions (from rule.c)
 *
 * These are declared static in rule.c; we re-declare them here as extern
 * so the rewrite pipeline can call them directly on the OptExpression tree
 * before Memo search begins.
 * ======================================================================== */

/* Predicate Pushdown (A group) */
extern List *pg_rule_pushdown_predicate_scan_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_pushdown_predicate_join_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_pushdown_predicate_agg_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_pushdown_predicate_project_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);

/* Column Pruning (B group) */
extern List *pg_rule_prune_scan_columns_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_prune_join_columns_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_prune_agg_columns_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_prune_project_columns_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_prune_sort_columns_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);

/* Join Reorder (C group) */
extern List *pg_rule_join_commutativity_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_join_associativity_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_join_left_asscom_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);

/* Limit Optimization (D group) */
extern List *pg_rule_merge_limit_with_sort_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_pushdown_limit_join_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);

/* Empty Set Pruning (E group) */
extern List *pg_rule_prune_empty_scan_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_prune_empty_join_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_prune_empty_union_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);

/* Aggregate Rewrite (F group) */
extern List *pg_rule_merge_two_agg_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_pushdown_agg_limit_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);

/* Join Simplification (G group) */
extern List *pg_rule_eliminate_join_with_constant_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_outer_join_elimination_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_inner_to_semi_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_merge_join_with_child_project_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);

/* Final Cleanup (H group) */
extern List *pg_rule_merge_project_with_child_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_eliminate_project_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_eliminate_sort_with_constant_key_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);
extern List *pg_rule_merge_filter_with_join_wrapper(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);

/* ========================================================================
 * Rewrite Pipeline Definition — 8 stages with actual rules
 * ======================================================================== */

static PgRewriteRule g_rules_predicate_pushdown[] = {
    {"PushDownPredicateScan",    PG_CASCADES_LOGICAL_FILTER, NULL, 0, 0.6},
    {"PushDownPredicateJoin",    PG_CASCADES_LOGICAL_FILTER, NULL, 0, 0.4},
    {"PushDownPredicateProject", PG_CASCADES_LOGICAL_FILTER, NULL, 0, 0.5},
    {"PushDownPredicateAgg",     PG_CASCADES_LOGICAL_FILTER, NULL, 0, 0.3},
    {NULL, 0, NULL, 0, 0.0}  /* sentinel */
};

static PgRewriteRule g_rules_column_prune[] = {
    {"PruneScanColumns",      PG_CASCADES_LOGICAL_SCAN,    NULL, 0, 0.6},
    {"PruneJoinColumns",      PG_CASCADES_LOGICAL_JOIN,    NULL, 0, 0.5},
    {"PruneAggColumns",       PG_CASCADES_LOGICAL_AGG,     NULL, 0, 0.5},
    {"PruneProjectColumns",   PG_CASCADES_LOGICAL_PROJECT, NULL, 0, 0.6},
    {"PruneSortColumns",      PG_CASCADES_LOGICAL_SORT,    NULL, 0, 0.5},
    {NULL, 0, NULL, 0, 0.0}
};

static PgRewriteRule g_rules_join_reorder[] = {
    {"JoinCommutativity",  PG_CASCADES_LOGICAL_JOIN, NULL, 0, 0.5},
    {"JoinAssociativity",  PG_CASCADES_LOGICAL_JOIN, NULL, 0, 0.2},
    {"JoinLeftAsscom",     PG_CASCADES_LOGICAL_JOIN, NULL, 0, 0.2},
    {NULL, 0, NULL, 0, 0.0}
};

static PgRewriteRule g_rules_limit_push[] = {
    {"MergeLimitWithSort", PG_CASCADES_LOGICAL_LIMIT, NULL, 0, 0.7},
    {"PushDownLimitJoin",  PG_CASCADES_LOGICAL_LIMIT, NULL, 0, 0.4},
    {NULL, 0, NULL, 0, 0.0}
};

static PgRewriteRule g_rules_agg_pushdown[] = {
    {"PushDownAggLimit", PG_CASCADES_LOGICAL_AGG, NULL, 0, 0.4},
    {"MergeTwoAgg",      PG_CASCADES_LOGICAL_AGG, NULL, 0, 0.5},
    {NULL, 0, NULL, 0, 0.0}
};

static PgRewriteRule g_rules_semijoin_dedup[] = {
    {"InnerToSemi",             PG_CASCADES_LOGICAL_JOIN, NULL, 0, 0.3},
    {"EliminateJoinWithConst",  PG_CASCADES_LOGICAL_JOIN, NULL, 0, 0.7},
    {"OuterJoinElimination",    PG_CASCADES_LOGICAL_JOIN, NULL, 0, 0.6},
    {"MergeJoinWithChildProj",  PG_CASCADES_LOGICAL_JOIN, NULL, 0, 0.4},
    {"MergeFilterWithJoin",     PG_CASCADES_LOGICAL_JOIN, NULL, 0, 0.4},
    {"PruneEmptyJoin",          PG_CASCADES_LOGICAL_JOIN, NULL, 0, 0.9},
    {"PruneEmptyUnion",         PG_CASCADES_LOGICAL_PROJECT, NULL, 0, 0.8},
    {NULL, 0, NULL, 0, 0.0}
};

static PgRewriteRule g_rules_final_cleanup[] = {
    {"MergeProjectWithChild",        PG_CASCADES_LOGICAL_PROJECT, NULL, 0, 0.5},
    {"EliminateProject",             PG_CASCADES_LOGICAL_PROJECT, NULL, 0, 0.6},
    {"EliminateSortWithConstKey",    PG_CASCADES_LOGICAL_SORT,    NULL, 0, 0.5},
    {"PruneEmptyScan",               PG_CASCADES_LOGICAL_SCAN,    NULL, 0, 0.9},
    {NULL, 0, NULL, 0, 0.0}
};

static PgRewriteStageDef g_rewrite_pipeline[REWRITE_NUM_STAGES] = {
    {REWRITE_CTE_INLINE,          "CTE Inline",           false, NULL, 0},
    {REWRITE_SUBQUERY,            "Subquery Rewrite",     false, NULL, 0},
    {REWRITE_PREDICATE_PUSHDOWN,  "Predicate Pushdown",   true,
     g_rules_predicate_pushdown,
     sizeof(g_rules_predicate_pushdown) / sizeof(PgRewriteRule) - 1},
    {REWRITE_COLUMN_PRUNE,        "Column Pruning",       true,
     g_rules_column_prune,
     sizeof(g_rules_column_prune) / sizeof(PgRewriteRule) - 1},
    {REWRITE_JOIN_REORDER,        "Join Reorder",         true,
     g_rules_join_reorder,
     sizeof(g_rules_join_reorder) / sizeof(PgRewriteRule) - 1},
    {REWRITE_LIMIT_PUSH,          "Limit Push/Optimize",  false,
     g_rules_limit_push,
     sizeof(g_rules_limit_push) / sizeof(PgRewriteRule) - 1},
    {REWRITE_AGG_PUSHDOWN,        "Aggregate Pushdown",   false,
     g_rules_agg_pushdown,
     sizeof(g_rules_agg_pushdown) / sizeof(PgRewriteRule) - 1},
    {REWRITE_SEMIJOIN_DEDUP,      "Semi-Join Dedup",      true,
     g_rules_semijoin_dedup,
     sizeof(g_rules_semijoin_dedup) / sizeof(PgRewriteRule) - 1},
};

/*
 * Note: g_rules_final_cleanup is applied as a 9th stage (post-pipeline)
 * in pg_cascades_logical_rewrite.
 */

/* ========================================================================
 * Helper: Look up a transform function by rule name
 * ======================================================================== */

/*
 * pg_rewrite_lookup_transform:
 *   Look up the transform function for a named rule.
 *   Scans all Phase 5 transformation rules to find the match.
 *   Returns NULL if the rule name is not found.
 */
static PgRuleTransformFn
pg_rewrite_lookup_transform(const char *name, PgCascadesOpKind *match_op)
{
    int num_rules;
    PgRule *rules;
    int i;

    /* Scan Phase 5 transformation rules */
    rules = pg_cascades_get_trans_rules_phase5(&num_rules);
    for (i = 0; i < num_rules; i++)
    {
        if (strcmp(rules[i].name, name) == 0)
        {
            if (match_op != NULL)
                *match_op = rules[i].from_op;
            return rules[i].transform;
        }
    }

    /* Also scan Phase 3 rules (JoinCommutativity) */
    rules = pg_cascades_get_trans_rules(&num_rules);
    for (i = 0; i < num_rules; i++)
    {
        if (strcmp(rules[i].name, name) == 0)
        {
            if (match_op != NULL)
                *match_op = rules[i].from_op;
            return rules[i].transform;
        }
    }

    return NULL;
}

/* ========================================================================
 * Rewrite tree traversal
 * ======================================================================== */

/*
 * pg_rewrite_apply_rules_to_expr:
 *   Apply a set of rules to a single expression.
 *   For each matching rule, call transform() and insert results.
 *   Returns true if any rule produced a new expression.
 */
static bool
pg_rewrite_apply_rules_to_expr(PgPlannerCascadesContext *ctx,
                                PgMemoGroup *group,
                                PgGroupExpr *expr,
                                PgRewriteRule *rules,
                                int num_rules)
{
    int i;
    bool changed = false;

    for (i = 0; i < num_rules; i++)
    {
        PgRewriteRule *rr = &rules[i];
        PgRuleTransformFn transform;
        PgCascadesOpKind match_op;
        List *new_exprs;
        ListCell *lc;

        /* Look up the actual transform function */
        transform = pg_rewrite_lookup_transform(rr->name, &match_op);
        if (transform == NULL)
        {
            if (ctx->debug)
                elog(NOTICE, "Cascades rewrite: rule '%s' not found in rule registry",
                     rr->name);
            continue;
        }

        /* Check if rule matches this expression */
        if (expr->op != match_op)
            continue;

        /* Apply the rule */
        new_exprs = transform(ctx, expr);
        if (new_exprs == NIL)
            continue;

        /* Insert new expressions into Memo */
        foreach(lc, new_exprs)
        {
            PgGroupExpr *new_expr = (PgGroupExpr *) lfirst(lc);
            PgMemoGroup *new_group;

            new_group = pg_memo_insert_expression(ctx, ctx->memo,
                                                   new_expr, group);
            if (new_group != NULL && new_group != group)
            {
                changed = true;
                if (ctx->debug)
                    elog(NOTICE, "Cascades rewrite: rule '%s' produced new group %d",
                         rr->name, new_group->id);
            }
        }

        /* Mark rule as explored on source expression */
        if (rr->rule_bit > 0)
            expr->explored_rules = bms_add_member(expr->explored_rules,
                                                   rr->rule_bit);
    }

    return changed;
}

/*
 * pg_rewrite_apply_rules_recursive:
 *   Recursively apply rules to all expressions in a group tree.
 *   Traverses children first (bottom-up), then applies rules to root.
 *   Returns true if any expression was modified.
 */
static bool
pg_rewrite_apply_rules_recursive(PgPlannerCascadesContext *ctx,
                                  PgMemoGroup *group,
                                  PgRewriteRule *rules,
                                  int num_rules)
{
    ListCell *lc;
    bool changed = false;

    if (group == NULL)
        return false;

    /*
     * Step 1: Recursively apply rules to child groups first (bottom-up).
     * We iterate over a snapshot of logical_exprs because the list may
     * grow as we apply rules.
     */
    foreach(lc, group->logical_exprs)
    {
        PgGroupExpr *expr = (PgGroupExpr *) lfirst(lc);
        ListCell *ic;

        foreach(ic, expr->inputs)
        {
            PgMemoGroup *child = (PgMemoGroup *) lfirst(ic);
            if (pg_rewrite_apply_rules_recursive(ctx, child, rules, num_rules))
                changed = true;
        }
    }

    /*
     * Step 2: Apply rules to this group's expressions.
     * We iterate over a stable list reference (list_copy) because
     * pg_memo_insert_expression may add new entries to logical_exprs.
     */
    {
        List *exprs_snapshot = list_copy(group->logical_exprs);

        foreach(lc, exprs_snapshot)
        {
            PgGroupExpr *expr = (PgGroupExpr *) lfirst(lc);

            if (pg_rewrite_apply_rules_to_expr(ctx, group, expr,
                                                rules, num_rules))
                changed = true;
        }

        list_free(exprs_snapshot);
    }

    return changed;
}

/* ========================================================================
 * Rewrite Phase Entry Point
 * ======================================================================== */

/*
 * pg_cascades_logical_rewrite:
 *   Execute the staged logical rewrite pipeline on the expression tree
 *   before Memo-based cost search.
 *
 *   For each stage:
 *     - If the stage has no rules, skip it (CTE Inline, Subquery).
 *     - If the stage is iterative: apply rules repeatedly until no more
 *       changes (convergence), up to a safety limit of 10 iterations.
 *     - If the stage is non-iterative: apply rules once.
 *
 *   A final cleanup stage (g_rules_final_cleanup) is always applied.
 *
 *   Returns PG_CASCADES_OK always (best-effort rewrite — errors are
 *   non-fatal: the cost-based search can still find good plans).
 */
PgCascadesStatus
pg_cascades_logical_rewrite(PgPlannerCascadesContext *ctx)
{
    int i;
    int total_rules_applied = 0;
    int stages_with_rules = 0;
    int stages_without_rules = 0;

    if (ctx->memo == NULL || ctx->memo->root_group == NULL)
    {
        if (ctx->debug)
            elog(NOTICE, "Cascades rewrite: no Memo/root group, skipping");
        return PG_CASCADES_OK;
    }

    for (i = 0; i < REWRITE_NUM_STAGES; i++)
    {
        PgRewriteStageDef *stage = &g_rewrite_pipeline[i];

        if (stage->rules == NULL || stage->num_rules == 0)
        {
            stages_without_rules++;
            if (ctx->debug)
                elog(NOTICE, "Cascades rewrite stage %d: %s (no rules, skipped)",
                     i, stage->name);
            continue;
        }

        stages_with_rules++;

        if (stage->iterate)
        {
            int iteration;
            int max_iterations = 10;  /* safety limit */

            for (iteration = 0; iteration < max_iterations; iteration++)
            {
                bool changed;

                changed = pg_rewrite_apply_rules_recursive(ctx,
                    ctx->memo->root_group, stage->rules, stage->num_rules);

                if (ctx->debug)
                    elog(NOTICE, "Cascades rewrite stage %d (%s) iteration %d: %s",
                         i, stage->name, iteration,
                         changed ? "changed" : "converged");

                if (!changed)
                    break;
            }

            total_rules_applied++;
        }
        else
        {
            pg_rewrite_apply_rules_recursive(ctx,
                ctx->memo->root_group, stage->rules, stage->num_rules);
            total_rules_applied++;

            if (ctx->debug)
                elog(NOTICE, "Cascades rewrite stage %d: %s (single-pass, %d rules)",
                     i, stage->name, stage->num_rules);
        }
    }

    /*
     * Apply final cleanup rules (g_rules_final_cleanup) once.
     * These are simple single-node transformations that don't need
     * a dedicated pipeline stage.
     */
    {
        int num_cleanup = sizeof(g_rules_final_cleanup) / sizeof(PgRewriteRule) - 1;
        pg_rewrite_apply_rules_recursive(ctx, ctx->memo->root_group,
                                          g_rules_final_cleanup, num_cleanup);
        if (ctx->debug)
            elog(NOTICE, "Cascades rewrite: final cleanup (%d rules)", num_cleanup);
    }

    if (ctx->debug)
        elog(NOTICE, "Cascades rewrite pipeline: %d stages with rules, "
             "%d stages without rules (total %d)",
             stages_with_rules, stages_without_rules, REWRITE_NUM_STAGES);

    return PG_CASCADES_OK;
}
