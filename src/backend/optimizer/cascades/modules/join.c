/* modules/join.c — Join operators (NestLoop, HashJoin, MergeJoin) + rules */
#include "postgres.h"
#include "optimizer/cascades.h"
void pg_module_join_init(void)
{
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_NESTLOOP, .name = "NestLoop",
    });
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_HASHJOIN, .name = "HashJoin",
    });
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_MERGEJOIN, .name = "MergeJoin",
    });

	pg_module_join_register_rules();
}

/* Rule declarations for join module */
/* Auto-generated from rule.c — do not edit by hand */
extern List *pg_rule_eliminate_join_with_constant(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_inner_to_semi(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_join_associativity(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_join_commutativity(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_join_left_asscom(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_join_to_hashjoin(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_join_to_hashjoin_phase4(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_join_to_mergejoin(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_join_to_mergejoin_phase4(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_join_to_nestloop(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_join_to_nestloop_phase4(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_merge_filter_with_join(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_merge_join_with_child_project(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_outer_join_elimination(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_prune_empty_join(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_prune_join_columns(PgPlannerCascadesContext *, PgGroupExpr *);
extern PgPattern *g_pat_join_filter_leaf_leaf;
extern PgPattern *g_pat_join_join_leaf_leaf_leaf;
extern PgPattern *g_pat_join_leaf_join_leaf_leaf;
extern PgPattern *g_pat_join_leaf_leaf;
extern PgPattern *g_pat_join_project_leaf_project_leaf;

void pg_module_join_register_rules(void)
{
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "EliminateJoinWithConst", PG_CASCADES_LOGICAL_JOIN, 0,
        NULL, pg_rule_eliminate_join_with_constant, 0.7, PG_RULE_BIT_ELIM_JOIN_CONST);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "InnerToSemi", PG_CASCADES_LOGICAL_JOIN, 0,
        NULL, pg_rule_inner_to_semi, 0.3, PG_RULE_BIT_INNER_TO_SEMI);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "JoinAssociativity", PG_CASCADES_LOGICAL_JOIN, 0,
        g_pat_join_join_leaf_leaf_leaf, pg_rule_join_associativity, 0.2, PG_RULE_BIT_JOIN_ASSOCIATIVITY);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "JoinCommutativity", PG_CASCADES_LOGICAL_JOIN, 0,
        g_pat_join_leaf_leaf, pg_rule_join_commutativity, 0.5, PG_RULE_BIT_JOIN_COMMUTATIVITY);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "JoinLeftAsscom", PG_CASCADES_LOGICAL_JOIN, 0,
        g_pat_join_leaf_join_leaf_leaf, pg_rule_join_left_asscom, 0.2, PG_RULE_BIT_JOIN_LEFT_ASSCOM);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalJoin->PhysicalHashJoin", PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_HASHJOIN,
        g_pat_join_leaf_leaf, pg_rule_join_to_hashjoin, 0.8, PG_RULE_BIT_JOIN_TO_HASHJOIN);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalJoin->PhysicalHashJoin_Phase4", PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_HASHJOIN,
        NULL, pg_rule_join_to_hashjoin_phase4, 0.9, PG_RULE_BIT_JOIN_TO_HASHJOIN_PHASE4);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalJoin->PhysicalMergeJoin", PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_MERGEJOIN,
        g_pat_join_leaf_leaf, pg_rule_join_to_mergejoin, 0.6, PG_RULE_BIT_JOIN_TO_MERGEJOIN);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalJoin->PhysicalMergeJoin_Phase4", PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_MERGEJOIN,
        NULL, pg_rule_join_to_mergejoin_phase4, 0.9, PG_RULE_BIT_JOIN_TO_MERGEJOIN_PHASE4);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalJoin->PhysicalNestLoop", PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_NESTLOOP,
        g_pat_join_leaf_leaf, pg_rule_join_to_nestloop, 0.5, PG_RULE_BIT_JOIN_TO_NESTLOOP);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalJoin->PhysicalNestLoop_Phase4", PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_NESTLOOP,
        NULL, pg_rule_join_to_nestloop_phase4, 0.9, PG_RULE_BIT_JOIN_TO_NESTLOOP_PHASE4);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "MergeFilterWithJoin", PG_CASCADES_LOGICAL_JOIN, 0,
        g_pat_join_filter_leaf_leaf, pg_rule_merge_filter_with_join, 0.4, PG_RULE_BIT_MERGE_FILTER_JOIN);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "MergeJoinWithChildProj", PG_CASCADES_LOGICAL_JOIN, 0,
        g_pat_join_project_leaf_project_leaf, pg_rule_merge_join_with_child_project, 0.4, PG_RULE_BIT_MERGE_JOIN_PROJ);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "OuterJoinElimination", PG_CASCADES_LOGICAL_JOIN, 0,
        NULL, pg_rule_outer_join_elimination, 0.6, PG_RULE_BIT_OUTER_JOIN_ELIM);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "PruneEmptyJoin", PG_CASCADES_LOGICAL_JOIN, 0,
        NULL, pg_rule_prune_empty_join, 0.9, PG_RULE_BIT_PRUNE_EMPTY_JOIN);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "PruneJoinColumns", PG_CASCADES_LOGICAL_JOIN, 0,
        NULL, pg_rule_prune_join_columns, 0.5, PG_RULE_BIT_PRUNE_JOIN_COLS);

	pg_module_join_register_rules();
}
