/*-------------------------------------------------------------------------
 * modules/union.c — UNION ALL operator module
 *   UNION ALL is handled by PG's standard Append/MergeAppend paths
 *   via IMPORTED_PATH.  This module registers the logical operator
 *   for future COMPOSABLE_OP support.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "optimizer/cascades.h"
#include "core/registry.h"

void pg_module_union_init(void)
{
    /* UNION ALL uses PG's standard Append path (IMPORTED_PATH mode).
     * No physical operator vtable needed yet — cost/build delegated
     * to create_plan() via the imported Path. */
}
