/*-------------------------------------------------------------------------
 * rewrite.c
 *    Cascades Rewrite Phase Pipeline (Phase 4a)
 *    Pre-Memo rule-based logical rewriting in stages.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"

/* ========================================================================
 * Rewrite Pipeline Definition
 *
 * Each stage applies a set of transformation rules to the logical
 * expression tree before cost-based Memo search begins.
 *
 * Stage order follows StarRocks: CTE inline → Subquery → PredPush →
 * ColPrune → JoinReorder → LimitPush → AggPush → SemiJoinDedup.
 *
 * Currently a skeleton — stages are defined but no rules are registered.
 * Rules will be added as transformation rules are implemented.
 * ======================================================================== */

static PgRewriteStageDef g_rewrite_pipeline[REWRITE_NUM_STAGES] = {
    {REWRITE_CTE_INLINE,          "CTE Inline",           false},
    {REWRITE_SUBQUERY,            "Subquery Rewrite",     false},
    {REWRITE_PREDICATE_PUSHDOWN,  "Predicate Pushdown",   true},
    {REWRITE_COLUMN_PRUNE,        "Column Pruning",       true},
    {REWRITE_JOIN_REORDER,        "Join Reorder",         true},
    {REWRITE_LIMIT_PUSH,          "Limit Push/Optimize",  false},
    {REWRITE_AGG_PUSHDOWN,        "Aggregate Pushdown",   false},
    {REWRITE_SEMIJOIN_DEDUP,      "Semi-Join Dedup",      false},
};

/* ========================================================================
 * Rewrite Phase Entry Point
 * ======================================================================== */

/*
 * pg_cascades_logical_rewrite:
 *   Execute the staged logical rewrite pipeline on the expression tree
 *   before Memo-based cost search.
 *
 *   Returns PG_CASCADES_OK always (best-effort rewrite, errors are
 *   non-fatal — the cost-based search can still find good plans).
 *
 *   Currently a skeleton: iterates stages but applies no rules.
 *   The pipeline structure is in place for future rule registration.
 */
PgCascadesStatus
pg_cascades_logical_rewrite(PgPlannerCascadesContext *ctx)
{
    int i;
    int stages_executed = 0;
    int stages_iterated = 0;

    for (i = 0; i < REWRITE_NUM_STAGES; i++)
    {
        PgRewriteStageDef *stage = &g_rewrite_pipeline[i];

        /*
         * Future: apply rules registered for this stage.
         *
         * For iterative stages (stage->iterate = true):
         *   Apply rules repeatedly until no more changes (convergence).
         *
         * For non-iterative stages:
         *   Apply rules once.
         *
         * Currently all stages are no-ops because no transformation
         * rules are registered yet.
         */

        if (stage->iterate)
            stages_iterated++;
        else
            stages_executed++;

        if (ctx->debug)
            elog(NOTICE, "Cascades rewrite stage %d: %s (%s)",
                 i, stage->name,
                 stage->iterate ? "iterative" : "single-pass");
    }

    if (ctx->debug)
        elog(NOTICE, "Cascades rewrite pipeline: %d stages executed, "
             "%d iterative stages (total %d)",
             stages_executed, stages_iterated, REWRITE_NUM_STAGES);

    return PG_CASCADES_OK;
}
