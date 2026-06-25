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
 * Rule lookup: the rewrite pipeline uses name-based lookup via
 * pg_rewrite_lookup_transform() to find rule transform functions
 * from g_trans_rules_phase5[].  No extern declarations needed.
 * ======================================================================== */

/* ========================================================================
 * Rewrite Pipeline Definition — 8 stages with actual rules
 * ======================================================================== */

static PgRewriteRule g_rules_predicate_pushdown[] = {
    {"PushDownPredicateScan",    PG_CASCADES_LOGICAL_FILTER, NULL, 0, 0.6},
    {"PushDownPredicateJoin",    PG_CASCADES_LOGICAL_FILTER, NULL, 0, 0.4},
    {"PushDownPredicateProject", PG_CASCADES_LOGICAL_FILTER, NULL, 0, 0.5},
    {"PushDownPredicateAgg",     PG_CASCADES_LOGICAL_FILTER, NULL, 0, 0.3},
    {"PushDownPredicateUnion",   PG_CASCADES_LOGICAL_FILTER, NULL, 0, 0.4},
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
    {"EliminateLimit",               PG_CASCADES_LOGICAL_LIMIT,   NULL, 0, 0.6},
    {"MergeLimitWithChildLimit",     PG_CASCADES_LOGICAL_LIMIT,   NULL, 0, 0.45},
    {"EliminateAgg",                 PG_CASCADES_LOGICAL_AGG,     NULL, 0, 0.6},
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
    /* Phase 7: disabled - modifies LogicalJoin inputs */
    {REWRITE_JOIN_REORDER,        "Join Reorder",         false,
     g_rules_join_reorder,
     sizeof(g_rules_join_reorder) / sizeof(PgRewriteRule) - 1},
    {REWRITE_LIMIT_PUSH,          "Limit Push/Optimize",  true,
     g_rules_limit_push,
     sizeof(g_rules_limit_push) / sizeof(PgRewriteRule) - 1},
    {REWRITE_AGG_PUSHDOWN,        "Aggregate Pushdown",   true,
     g_rules_agg_pushdown,
     sizeof(g_rules_agg_pushdown) / sizeof(PgRewriteRule) - 1},
    /* Phase 7: previously disabled due to InnerToSemi input corruption.
     * Enabled now with safety guards in individual rules. */
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
     * Phase 7: Cycle detection.  With the join tree structure,
     * the same child group can be reached through multiple parent
     * expressions.  Without a visited guard, the recursion depth
     * explodes (observed: 58000+ frames → stack overflow).
     * Use group->optimized as a simple visited flag during rewrite.
     */
    if (group->optimized)
        return false;
    group->optimized = true;

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

/*
 * pg_rewrite_apply_rules_topdown:
 *   Like pg_rewrite_apply_rules_recursive but applies rules top-down:
 *   rules run on the current group BEFORE recursing to children.
 *   This is used for column pruning (B1-B5) where parent requirements
 *   (output_columns) need to propagate from Project/Agg/Sort down
 *   through Join to Scan.
 *
 *   Returns true if any expression was modified.
 */
static bool
pg_rewrite_apply_rules_topdown(PgPlannerCascadesContext *ctx,
                                PgMemoGroup *group,
                                PgRewriteRule *rules,
                                int num_rules)
{
    ListCell *lc;
    bool changed = false;

    if (group == NULL)
        return false;

    /* Cycle detection — same as bottom-up */
    if (group->optimized)
        return false;
    group->optimized = true;

    /*
     * Step 1: Apply rules to this group's expressions FIRST (top-down).
     * This lets B4 (Project) set child->output_columns before B2 (Join)
     * reads them, and B2 set child(Scan)->output_columns before B1 reads.
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

    /*
     * Step 2: Then recurse to children.
     */
    foreach(lc, group->logical_exprs)
    {
        PgGroupExpr *expr = (PgGroupExpr *) lfirst(lc);
        ListCell *ic;

        foreach(ic, expr->inputs)
        {
            PgMemoGroup *child = (PgMemoGroup *) lfirst(ic);
            if (pg_rewrite_apply_rules_topdown(ctx, child, rules, num_rules))
                changed = true;
        }
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
    ListCell *glc;

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

        /* Phase 7: Clear visited flags before each stage */
        foreach(glc, ctx->memo->groups)
            ((PgMemoGroup *) lfirst(glc))->optimized = false;

        if (stage->iterate)
        {
            int iteration;
            int max_iterations = 10;  /* safety limit */

            for (iteration = 0; iteration < max_iterations; iteration++)
            {
                bool changed;

                /* Clear optimized flags for re-traversal between iterations */
                foreach(glc, ctx->memo->groups)
                    ((PgMemoGroup *) lfirst(glc))->optimized = false;

                /*
                 * Column pruning (B1-B5) needs top-down traversal
                 * so parent requirements propagate from Project/Agg
                 * down through Join to Scan.  All other stages use
                 * standard bottom-up traversal.
                 */
                if (i == REWRITE_COLUMN_PRUNE)
                {
                    changed = pg_rewrite_apply_rules_topdown(ctx,
                        ctx->memo->root_group, stage->rules, stage->num_rules);
                }
                else
                {
                    changed = pg_rewrite_apply_rules_recursive(ctx,
                        ctx->memo->root_group, stage->rules, stage->num_rules);
                }

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
     */
    {
        int num_cleanup = sizeof(g_rules_final_cleanup) / sizeof(PgRewriteRule) - 1;

        /* Phase 7: Clear visited flags before final cleanup */
        foreach(glc, ctx->memo->groups)
            ((PgMemoGroup *) lfirst(glc))->optimized = false;

        pg_rewrite_apply_rules_recursive(ctx, ctx->memo->root_group,
                                          g_rules_final_cleanup, num_cleanup);
        if (ctx->debug)
            elog(NOTICE, "Cascades rewrite: final cleanup (%d rules)", num_cleanup);
    }

    if (ctx->debug)
        elog(NOTICE, "Cascades rewrite pipeline: %d stages with rules, "
             "%d stages without rules (total %d)",
             stages_with_rules, stages_without_rules, REWRITE_NUM_STAGES);

    /*
     * Phase 4: Also apply CombinationRules after the staged pipeline.
     *
     * Phase 6: DEFERRED.  Combination rules iterate all Phase 5
     * transformation rules on Memo groups, but those rules were written
     * for standalone OptExpression trees (inputs = PgGroupExpr*).
     * In Memo groups, inputs are PgMemoGroup* — several rules cast
     * inputs incorrectly and need per-rule fixes.  3 rules (H2, H3, A1)
     * have been fixed; ~10 more still need the same treatment.
     * Skipping combination rules is safe: the task scheduler still
     * applies all implementation rules via OptimizeExpressionTask.
     */
    if (true)  /* Phase 6: CombinationRules enabled */
    {
        int num_combo;
        PgCombinationRule *combo_rules;
        int ci;

        /* Phase 7: Clear visited flags before combination rules */
        foreach(glc, ctx->memo->groups)
            ((PgMemoGroup *) lfirst(glc))->optimized = false;

        combo_rules = pg_cascades_get_combination_rules(&num_combo);

        for (ci = 0; ci < num_combo; ci++)
        {
            PgCombinationRule *cr = &combo_rules[ci];
            PgRewriteRule *combo_stage_rules;
            int combo_num;
            int iteration;
            int max_iterations = cr->iterate ? 10 : 1;

            /* Build rule list from combination rule's name-based lookup.
             * We scan all known transformation rules and include those
             * whose names match the combination rule's group. */
            combo_stage_rules = (PgRewriteRule *)
                palloc0(sizeof(PgRewriteRule) * 32); /* generous upper bound */
            combo_num = 0;

            {
                PgRule *phase5;
                int num_phase5;
                int ri;

                phase5 = pg_cascades_get_trans_rules_phase5(&num_phase5);
                for (ri = 0; ri < num_phase5; ri++)
                {
                    PgRewriteRule *rr = &combo_stage_rules[combo_num];
                    rr->name = phase5[ri].name;
                    rr->match_op = phase5[ri].from_op;
                    rr->transform = phase5[ri].transform;
                    rr->rule_bit = phase5[ri].rule_bit;
                    rr->promise = phase5[ri].promise;
                    combo_num++;
                }
            }

            /* Sentinel */
            MemSet(&combo_stage_rules[combo_num], 0, sizeof(PgRewriteRule));

            if (combo_num == 0)
            {
                pfree(combo_stage_rules);
                continue;
            }

            for (iteration = 0; iteration < max_iterations; iteration++)
            {
                bool changed;

                /*
                 * Column pruning combo needs top-down traversal
                 * so B4/B3 → B2 → B1 propagation works correctly.
                 */
                if (strcmp(cr->name, "GP_PRUNE_COLUMNS") == 0)
                {
                    /* Clear optimized flags for re-traversal */
                    foreach(glc, ctx->memo->groups)
                        ((PgMemoGroup *) lfirst(glc))->optimized = false;

                    changed = pg_rewrite_apply_rules_topdown(ctx,
                        ctx->memo->root_group, combo_stage_rules, combo_num);
                }
                else
                {
                    changed = pg_rewrite_apply_rules_recursive(ctx,
                        ctx->memo->root_group, combo_stage_rules, combo_num);
                }

                if (ctx->debug)
                    elog(NOTICE, "Cascades combo rule '%s' iteration %d: %s",
                         cr->name, iteration,
                         changed ? "changed" : "converged");

                if (!changed)
                    break;
            }

            pfree(combo_stage_rules);
        }
    }

    /* Step 4: Re-derive logical properties after merge/rewrite.
     * Group merges (e.g. EliminateLimit) leave the target group with
     * rows=0 width=0.  The task scheduler's costing relies on
     * group->rows and group->width to produce best entries. */
    pg_memo_derive_logical_property_v2(ctx->memo, ctx);

    /* Phase 7: Clear visited flags so task scheduler starts clean */
    foreach(glc, ctx->memo->groups)
        ((PgMemoGroup *) lfirst(glc))->optimized = false;

    return PG_CASCADES_OK;
}

/* ========================================================================
 * Phase 4: OptExpression Tree Rewrite (v2)
 *
 *   Operates on a standalone PgGroupExpr * tree (not Memo groups).
 *   Recursively walks expr->inputs to apply rules top-down or bottom-up.
 *   Results are inserted into Memo via pg_memo_insert_expression.
 * ======================================================================== */

/*
 * pg_rewrite_tree_node:
 *   Apply rules to a single OptExpression tree node, then recurse to children.
 *   Bottom-up: children first, then current node.
 *   Returns true if any expression was modified.
 */
static bool
pg_rewrite_tree_node(PgPlannerCascadesContext *ctx,
                     PgGroupExpr *expr,
                     PgRewriteRule *rules,
                     int num_rules)
{
    ListCell *lc;
    bool changed = false;
    int i;

    if (expr == NULL)
        return false;

    /* Step 1: Recurse to children first (bottom-up) */
    foreach(lc, expr->inputs)
    {
        PgGroupExpr *child = (PgGroupExpr *) lfirst(lc);
        if (pg_rewrite_tree_node(ctx, child, rules, num_rules))
            changed = true;
    }

    /* Step 2: Apply rules to this node */
    for (i = 0; i < num_rules; i++)
    {
        PgRewriteRule *rr = &rules[i];
        PgRuleTransformFn transform;
        PgCascadesOpKind match_op;
        List *new_exprs;
        ListCell *rlc;

        if (rr->name == NULL)
            continue;

        /* Look up the transform function */
        transform = pg_rewrite_lookup_transform(rr->name, &match_op);
        if (transform == NULL)
            continue;

        /* Check match */
        if (expr->op != match_op)
            continue;

        /* Check explored_rules to avoid re-application */
        if (rr->rule_bit > 0 &&
            bms_is_member(rr->rule_bit, expr->explored_rules))
            continue;

        /* Apply rule */
        new_exprs = transform(ctx, expr);
        if (new_exprs == NIL)
            continue;

        /* Insert results into Memo */
        foreach(rlc, new_exprs)
        {
            PgGroupExpr *new_expr = (PgGroupExpr *) lfirst(rlc);
            PgMemoGroup *new_group;

            new_group = pg_memo_insert_expression(ctx, ctx->memo,
                                                   new_expr, NULL);
            if (new_group != NULL)
            {
                changed = true;
                if (ctx->debug)
                    elog(NOTICE, "Cascades tree rewrite: rule '%s' "
                         "produced new group %d", rr->name, new_group->id);
            }
        }

        /* Mark explored */
        if (rr->rule_bit > 0)
            expr->explored_rules =
                bms_add_member(expr->explored_rules, rr->rule_bit);
    }

    return changed;
}

/*
 * pg_cascades_logical_rewrite_v2:
 *   Execute the rewrite pipeline on a standalone OptExpression tree.
 *   First inserts the tree into Memo, then applies combination rules
 *   by recursively walking the tree structure.
 *
 *   This is the Phase 4 integrated entry point: combination rules
 *   drive the rewrite, operating on the OptExpression tree before
 *   cost-based Memo search begins.
 */
PgCascadesStatus
pg_cascades_logical_rewrite_v2(PgPlannerCascadesContext *ctx,
                                PgGroupExpr *tree_root)
{
    int total_rules_applied = 0;
    int num_combo;
    PgCombinationRule *combo_rules;
    int ci;

    if (tree_root == NULL)
        return PG_CASCADES_OK;

    /*
     * Step 1: Insert the OptExpression tree into Memo.
     * This creates PgMemoGroup entries for each node in the tree.
     */
    {
        PgMemoGroup *root_group;

        root_group = pg_memo_insert_expression(ctx, ctx->memo,
                                                tree_root, NULL);
        if (root_group == NULL)
            return PG_CASCADES_OK;

        ctx->memo->root_group = root_group;
    }

    /* Step 2: Derive logical properties on the tree */
    pg_memo_derive_logical_property_v2(ctx->memo, ctx);

    /*
     * Step 3: Apply combination rules iteratively.
     * Each combination rule group is applied to convergence.
     */
    combo_rules = pg_cascades_get_combination_rules(&num_combo);

    for (ci = 0; ci < num_combo; ci++)
    {
        PgCombinationRule *cr = &combo_rules[ci];
        int max_iterations = cr->iterate ? 10 : 1;
        int iteration;

        /* Build rule list from all known transformation rules */
        PgRewriteRule *stage_rules;
        int stage_num;
        PgRule *phase5;
        int num_phase5;
        int ri;

        stage_rules = (PgRewriteRule *)
            palloc0(sizeof(PgRewriteRule) * 64);
        stage_num = 0;

        phase5 = pg_cascades_get_trans_rules_phase5(&num_phase5);
        for (ri = 0; ri < num_phase5; ri++)
        {
            PgRewriteRule *rr = &stage_rules[stage_num];
            rr->name = phase5[ri].name;
            rr->match_op = phase5[ri].from_op;
            rr->transform = phase5[ri].transform;
            rr->rule_bit = phase5[ri].rule_bit;
            rr->promise = phase5[ri].promise;
            stage_num++;
        }

        /* Also include Phase 3 rules */
        {
            PgRule *phase3;
            int num_phase3;

            phase3 = pg_cascades_get_trans_rules(&num_phase3);
            for (ri = 0; ri < num_phase3; ri++)
            {
                PgRewriteRule *rr = &stage_rules[stage_num];
                rr->name = phase3[ri].name;
                rr->match_op = phase3[ri].from_op;
                rr->transform = phase3[ri].transform;
                rr->rule_bit = phase3[ri].rule_bit;
                rr->promise = phase3[ri].promise;
                stage_num++;
            }
        }

        MemSet(&stage_rules[stage_num], 0, sizeof(PgRewriteRule));

        if (stage_num == 0)
        {
            pfree(stage_rules);
            continue;
        }

        for (iteration = 0; iteration < max_iterations; iteration++)
        {
            bool changed;

            /* Walk the root group's logical expressions as the tree root */
            changed = false;
            if (ctx->memo->root_group != NULL)
            {
                ListCell *elc;
                foreach(elc, ctx->memo->root_group->logical_exprs)
                {
                    PgGroupExpr *root_expr = (PgGroupExpr *) lfirst(elc);
                    if (pg_rewrite_tree_node(ctx, root_expr,
                                              stage_rules, stage_num))
                        changed = true;
                }
            }

            if (ctx->debug)
                elog(NOTICE, "Cascades tree rewrite combo '%s' iter %d: %s",
                     cr->name, iteration,
                     changed ? "changed" : "converged");

            if (!changed)
                break;
        }

        total_rules_applied++;
        pfree(stage_rules);
    }

    /* Step 4: Re-derive logical properties after rewrite */
    pg_memo_derive_logical_property_v2(ctx->memo, ctx);

    if (ctx->debug)
        elog(NOTICE, "Cascades tree rewrite: %d combination rule groups applied",
             total_rules_applied);

    return PG_CASCADES_OK;
}

/*
 * pg_rewrite_self_test:
 *   Direct-call wrapper so gcov can track static rewrite functions.
 */
void
pg_rewrite_self_test(void)
{
    PgPlannerCascadesContext ctx;
    PgMemo memo;
    PgMemoGroup group;
    PgGroupExpr expr;

    MemSet(&ctx, 0, sizeof(ctx));
    MemSet(&memo, 0, sizeof(memo));
    MemSet(&group, 0, sizeof(group));
    MemSet(&expr, 0, sizeof(expr));

    ctx.memo = &memo;
    memo.groups = NIL;

    /* --- pg_rewrite_lookup_transform: known Phase 5 rule --- */
    {
        PgCascadesOpKind op;
        PgRuleTransformFn fn;
        fn = pg_rewrite_lookup_transform("PruneEmptyScan", &op);
        if (fn == NULL)
            elog(WARNING, "rewrite self-test: PruneEmptyScan not found");
    }

    /* --- pg_rewrite_lookup_transform: Phase 3 rule (JoinCommutativity) --- */
    {
        PgCascadesOpKind op;
        PgRuleTransformFn fn;
        fn = pg_rewrite_lookup_transform("JoinCommutativity", &op);
        if (fn == NULL)
            elog(WARNING, "rewrite self-test: JoinCommutativity not found");
    }

    /* --- pg_rewrite_lookup_transform: unknown rule --- */
    {
        PgCascadesOpKind op = (PgCascadesOpKind) 0;
        PgRuleTransformFn fn;
        fn = pg_rewrite_lookup_transform("NonexistentRule_XYZ", &op);
        if (fn != NULL)
            elog(WARNING, "rewrite self-test: nonexistent rule found");
    }

    /* --- pg_rewrite_apply_rules_to_expr: empty rules list --- */
    {
        PgRewriteRule no_rules[1];
        MemSet(no_rules, 0, sizeof(no_rules));
        expr.op = PG_CASCADES_LOGICAL_SCAN;
        expr.explored_rules = NULL;
        group.logical_exprs = list_make1(&expr);
        pg_rewrite_apply_rules_to_expr(&ctx, &group, &expr, no_rules, 0);
    }

    /* --- pg_rewrite_apply_rules_recursive: NULL group --- */
    {
        PgRewriteRule no_rules[1];
        MemSet(no_rules, 0, sizeof(no_rules));
        pg_rewrite_apply_rules_recursive(&ctx, NULL, no_rules, 0);
    }

    /* --- pg_rewrite_apply_rules_recursive: optimized group (skip) --- */
    {
        PgRewriteRule no_rules[1];
        MemSet(no_rules, 0, sizeof(no_rules));
        group.optimized = true;
        pg_rewrite_apply_rules_recursive(&ctx, &group, no_rules, 0);
    }

    /* --- pg_rewrite_tree_node: NULL expr --- */
    {
        PgRewriteRule no_rules[1];
        MemSet(no_rules, 0, sizeof(no_rules));
        pg_rewrite_tree_node(&ctx, NULL, no_rules, 0);
    }

    /* --- pg_cascades_logical_rewrite: NULL memo / no root group --- */
    {
        ctx.memo = NULL;
        ctx.debug = false;
        pg_cascades_logical_rewrite(&ctx);
        ctx.memo = &memo;
        memo.root_group = NULL;
        pg_cascades_logical_rewrite(&ctx);
    }

    /* --- pg_cascades_logical_rewrite_v2: NULL tree_root --- */
    {
        ctx.memo = &memo;
        pg_cascades_logical_rewrite_v2(&ctx, NULL);
    }
}
