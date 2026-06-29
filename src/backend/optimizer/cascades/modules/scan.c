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

	pg_module_scan_register_rules();
}

/* Rule declarations for scan module */
/* Auto-generated from rule.c — do not edit by hand */
extern List *pg_rule_prune_empty_scan(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_prune_scan_columns(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_scan_to_bitmapheapscan(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_scan_to_indexscan(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_scan_to_seqscan(PgPlannerCascadesContext *, PgGroupExpr *);

void pg_module_scan_register_rules(void)
{
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalScan->PhysicalBitmapHeapScan", PG_CASCADES_LOGICAL_SCAN, PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN,
        NULL, pg_rule_scan_to_bitmapheapscan, 0.7, PG_RULE_BIT_SCAN_TO_BITMAPSCAN);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalScan->PhysicalIndexScan", PG_CASCADES_LOGICAL_SCAN, PG_CASCADES_PHYSICAL_INDEXSCAN,
        NULL, pg_rule_scan_to_indexscan, 0.8, PG_RULE_BIT_SCAN_TO_INDEXSCAN);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalScan->PhysicalSeqScan", PG_CASCADES_LOGICAL_SCAN, PG_CASCADES_PHYSICAL_SEQSCAN,
        NULL, pg_rule_scan_to_seqscan, 0.5, PG_RULE_BIT_SCAN_TO_SEQSCAN);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "PruneEmptyScan", PG_CASCADES_LOGICAL_SCAN, 0,
        NULL, pg_rule_prune_empty_scan, 0.9, PG_RULE_BIT_PRUNE_EMPTY_SCAN);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "PruneScanColumns", PG_CASCADES_LOGICAL_SCAN, 0,
        NULL, pg_rule_prune_scan_columns, 0.6, PG_RULE_BIT_PRUNE_SCAN_COLS);

}
