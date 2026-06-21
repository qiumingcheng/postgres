/*-------------------------------------------------------------------------
 * debug.c
 *    Cascades 调试输出函数
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "utils/elog.h"

void
debug_print_cascades_memo(PgPlannerCascadesContext *ctx)
{
    ListCell   *lc;
    int         num_groups = list_length(ctx->memo->groups);
    int         num_logical = 0;
    int         num_physical = 0;

    elog(NOTICE, "=== Cascades Memo Dump ===");
    elog(NOTICE, "Total groups: %d", num_groups);

    foreach(lc, ctx->memo->groups)
    {
        PgMemoGroup *group = (PgMemoGroup *) lfirst(lc);

        num_logical += list_length(group->logical_exprs);
        num_physical += list_length(group->physical_exprs);

        elog(NOTICE, "Group %d: rows=%.0f width=%d logical=%d physical=%d best=%d",
             group->id, group->rows, group->width,
             list_length(group->logical_exprs),
             list_length(group->physical_exprs),
             list_length(group->best_entries));
    }
    elog(NOTICE, "Total logical exprs: %d", num_logical);
    elog(NOTICE, "Total physical exprs: %d", num_physical);
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
