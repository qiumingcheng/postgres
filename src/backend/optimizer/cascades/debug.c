/*-------------------------------------------------------------------------
 * debug.c
 *    Cascades 调试输出函数 (Phase 8: enhanced Memo dump + rule stats)
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "utils/elog.h"

/* ========================================================================
 * Memo dump
 * ======================================================================== */

void
debug_print_cascades_memo(PgPlannerCascadesContext *ctx)
{
    ListCell   *lc;
    int         num_groups = list_length(ctx->memo->groups);
    int         num_logical = 0;
    int         num_physical = 0;
    int         num_best = 0;

    elog(NOTICE, "=== Cascades Memo Dump ===");
    elog(NOTICE, "Total groups: %d", num_groups);

    foreach(lc, ctx->memo->groups)
    {
        PgMemoGroup *group = (PgMemoGroup *) lfirst(lc);
        int n_log = list_length(group->logical_exprs);
        int n_phy = list_length(group->physical_exprs);
        int n_best = list_length(group->best_entries);

        num_logical += n_log;
        num_physical += n_phy;
        num_best += n_best;

        elog(NOTICE, "Group %d: rows=%.0f width=%d logical=%d physical=%d best=%d%s",
             group->id, group->rows, group->width,
             n_log, n_phy, n_best,
             group->rel != NULL ? " [has RelOptInfo]" : "");

        /* Phase 8: show best entry costs */
        if (n_best > 0)
        {
            ListCell *bc;
            foreach(bc, group->best_entries)
            {
                PgGroupBestEntry *entry = (PgGroupBestEntry *) lfirst(bc);
                elog(NOTICE, "  best: op=%d startup=%.4f total=%.4f",
                     entry->expr->op,
                     entry->startup_cost, entry->total_cost);
            }
        }
    }
    elog(NOTICE, "Total: logical=%d physical=%d best=%d",
         num_logical, num_physical, num_best);
}

/* ========================================================================
 * Rule statistics (Phase 8)
 * ======================================================================== */

void
debug_print_cascades_rules(PgPlannerCascadesContext *ctx)
{
    elog(NOTICE, "=== Cascades Rules ===");
    elog(NOTICE, "Implementation rules: %d", ctx->num_impl_rules);
    elog(NOTICE, "Transformation rules: %d", ctx->num_trans_rules);
    elog(NOTICE, "Total rules: %d", ctx->num_impl_rules + ctx->num_trans_rules);
    elog(NOTICE, "Tasks executed: %d", ctx->num_tasks_executed);
    elog(NOTICE, "Upper bound cost: %.4f", ctx->upper_bound_cost);
}

void
debug_print_cascades_fallback_reason(PgPlannerCascadesContext *ctx,
                                      const char *reason)
{
    if (ctx != NULL && ctx->debug)
        elog(NOTICE, "Cascades fallback: %s", reason);
    else if (ctx == NULL && cascades_planner_debug)
        elog(NOTICE, "Cascades fallback: %s", reason);
}
