/*-------------------------------------------------------------------------
 * modules/scan.c — Scan operators (SeqScan, IndexScan, BitmapHeapScan)
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "optimizer/cascades.h"

/* all scan transform functions are in rule.c — non-static, visible */

void pg_module_scan_init(void)
{
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_SEQSCAN, .name = "SeqScan",
    });
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_INDEXSCAN, .name = "IndexScan",
    });
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN, .name = "BitmapHeapScan",
    });
}
