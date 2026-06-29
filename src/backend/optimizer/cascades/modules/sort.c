#include "postgres.h"
#include "miscadmin.h"
#include "optimizer/planmain.h"
#include "optimizer/cascades.h"
#include "optimizer/cost.h"
static void pg_cost_sort(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
    PgGroupExpr *expr, double input_rows, int input_width,
    Cost child_startup, Cost child_total,
    Cost *out_startup, Cost *out_total)
{
    Path dummy_path;
    PgSortPrivate *sp = (PgSortPrivate *) expr->op_private;
    double lt = (sp && sp->limit_tuples > 0) ? sp->limit_tuples : -1.0;
    MemSet(&dummy_path, 0, sizeof(Path));
    dummy_path.pathtype = T_Sort;
    cost_sort(&dummy_path, ctx->root, (sp ? sp->pathkeys : NIL),
        child_total, input_rows, input_width, 0.0, work_mem, lt);
    *out_startup = dummy_path.startup_cost;
    *out_total   = dummy_path.total_cost;

}
static Plan *pg_build_sort(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
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
    return (Plan *) make_sort_from_pathkeys(ctx->root, child,
        required->pathkeys, required->limit_tuples);

}
void pg_module_sort_init(void)
{
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_SORT, .name = "Sort",
        .cost_fn = pg_cost_sort, .build_plan_fn = pg_build_sort,
    });

	pg_module_sort_register_rules();
}

/* Rule declarations for sort module */
/* Auto-generated from rule.c — do not edit by hand */
extern List *pg_rule_eliminate_sort_with_constant_key(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_enforce_sort(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_prune_sort_columns(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_sort_to_physical_sort(PgPlannerCascadesContext *, PgGroupExpr *);

void pg_module_sort_register_rules(void)
{
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "EliminateSortWithConstKey", PG_CASCADES_LOGICAL_SORT, 0,
        NULL, pg_rule_eliminate_sort_with_constant_key, 0.5, PG_RULE_BIT_ELIM_SORT_CONST_KEY);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "EnforceSort", 0, 0,
        NULL, pg_rule_enforce_sort, 0.0, PG_RULE_BIT_ENFORCE_SORT);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalSort->PhysicalSort", PG_CASCADES_LOGICAL_SORT, PG_CASCADES_PHYSICAL_SORT,
        NULL, pg_rule_sort_to_physical_sort, 1.0, PG_RULE_BIT_SORT_TO_SORT);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "PruneSortColumns", PG_CASCADES_LOGICAL_SORT, 0,
        NULL, pg_rule_prune_sort_columns, 0.5, PG_RULE_BIT_PRUNE_SORT_COLS);

}
