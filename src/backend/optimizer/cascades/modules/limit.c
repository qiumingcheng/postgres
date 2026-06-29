#include "postgres.h"
#include "optimizer/planmain.h"
#include "optimizer/cascades.h"
static void pg_cost_limit(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
    PgGroupExpr *expr, double input_rows, int input_width,
    Cost child_startup, Cost child_total,
    Cost *out_startup, Cost *out_total)
{
    double frac = 1.0;
    if (ctx->upper->limit_tuples > 0 && ctx->upper->limit_tuples < input_rows)
        frac = ctx->upper->limit_tuples / input_rows;
    *out_startup = child_startup;
    *out_total = child_startup + (child_total - child_startup) * frac;

}
static Plan *pg_build_limit(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
    PgGroupExpr *expr, PgGroupBestEntry *best,
    PgRequiredProperty *required, PgOutputProperty *output)
{
    Plan *child; PgGroupBestEntry *child_best = NULL;
    PgOutputProperty child_out; PgMemoGroup *child_group; ListCell *lc;
    if (expr->inputs == NIL) return NULL;
    child_group = (PgMemoGroup *) linitial(expr->inputs);
    if (child_group->best_entries == NIL) return NULL;
    foreach(lc, child_group->best_entries) {
        PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
        if (best->child_required_props != NIL &&
            pg_required_property_equal(e->required, pg_safe_linitial_child_req(best)))
        { child_best = e; break; }
    }
    if (child_best == NULL) return NULL;
    child = pg_cascades_build_plan_recurse(ctx, child_group,
        pg_safe_linitial_child_req(best), child_best, &child_out);
    if (child == NULL) return NULL;
    *output = best->output;
    return (Plan *) make_limit(child, ctx->root->parse->limitOffset,
        ctx->root->parse->limitCount, ctx->upper->offset_est, ctx->upper->count_est);

}
void pg_module_limit_init(void) {
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_LIMIT, .name = "Limit",
        .cost_fn = pg_cost_limit, .build_plan_fn = pg_build_limit,
    });

	pg_module_limit_register_rules();
}

/* Rule declarations for limit module */
/* Auto-generated from rule.c — do not edit by hand */
extern List *pg_rule_eliminate_limit(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_limit_to_physical_limit(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_merge_limit_with_child_limit(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_merge_limit_with_sort(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_pushdown_limit_join(PgPlannerCascadesContext *, PgGroupExpr *);
extern PgPattern *g_pat_limit_join_leaf_leaf;
extern PgPattern *g_pat_limit_sort_leaf;

void pg_module_limit_register_rules(void)
{
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "EliminateLimit", PG_CASCADES_LOGICAL_LIMIT, 0,
        NULL, pg_rule_eliminate_limit, 0.6, PG_RULE_BIT_ELIMINATE_LIMIT);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalLimit->PhysicalLimit", PG_CASCADES_LOGICAL_LIMIT, PG_CASCADES_PHYSICAL_LIMIT,
        NULL, pg_rule_limit_to_physical_limit, 1.0, PG_RULE_BIT_LIMIT_TO_LIMIT);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "MergeLimitWithChildLimit", PG_CASCADES_LOGICAL_LIMIT, 0,
        NULL, pg_rule_merge_limit_with_child_limit, 0.45, PG_RULE_BIT_MERGE_LIMIT_CHILD_LIMIT);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "MergeLimitWithSort", PG_CASCADES_LOGICAL_LIMIT, 0,
        g_pat_limit_sort_leaf, pg_rule_merge_limit_with_sort, 0.7, PG_RULE_BIT_MERGE_LIMIT_SORT);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "PushDownLimitJoin", PG_CASCADES_LOGICAL_LIMIT, 0,
        g_pat_limit_join_leaf_leaf, pg_rule_pushdown_limit_join, 0.4, PG_RULE_BIT_PUSHDOWN_LIMIT_JOIN);

}
