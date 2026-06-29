/* modules/filter.c — Filter + predicate pushdown rules */
#include "postgres.h"
#include "optimizer/cascades.h"
void pg_module_filter_init(void) {}

/* Rule declarations for filter module */
/* Auto-generated from rule.c — do not edit by hand */
extern List *pg_rule_pushdown_predicate_agg(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_pushdown_predicate_join(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_pushdown_predicate_project(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_pushdown_predicate_scan(PgPlannerCascadesContext *, PgGroupExpr *);
extern PgPattern *g_pat_filter_join_leaf_leaf;
extern PgPattern *g_pat_filter_project_leaf;

void pg_module_filter_register_rules(void)
{
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "PushDownPredicateAgg", PG_CASCADES_LOGICAL_FILTER, 0,
        NULL, pg_rule_pushdown_predicate_agg, 0.3, PG_RULE_BIT_PUSHDOWN_PRED_AGG);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "PushDownPredicateJoin", PG_CASCADES_LOGICAL_FILTER, 0,
        g_pat_filter_join_leaf_leaf, pg_rule_pushdown_predicate_join, 0.4, PG_RULE_BIT_PUSHDOWN_PRED_JOIN);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "PushDownPredicateProject", PG_CASCADES_LOGICAL_FILTER, 0,
        g_pat_filter_project_leaf, pg_rule_pushdown_predicate_project, 0.5, PG_RULE_BIT_PUSHDOWN_PRED_PROJ);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "PushDownPredicateScan", PG_CASCADES_LOGICAL_FILTER, 0,
        NULL, pg_rule_pushdown_predicate_scan, 0.6, PG_RULE_BIT_PUSHDOWN_PRED_SCAN);

	pg_module_filter_register_rules();
}
