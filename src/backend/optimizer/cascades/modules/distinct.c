/* modules/distinct.c — Distinct/Unique operator (cost + build from planbuild.c) */
#include "postgres.h"
#include "miscadmin.h"
#include "optimizer/planmain.h"
#include "optimizer/cascades.h"
#include "optimizer/cost.h"

static void
pg_cost_unique(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
    PgGroupExpr *expr, double input_rows, int input_width,
    Cost child_startup, Cost child_total,
    Cost *out_startup, Cost *out_total)
{
    Path dummy_path;
    MemSet(&dummy_path, 0, sizeof(Path));
    dummy_path.pathtype = T_Unique;
    cost_sort(&dummy_path, ctx->root, ctx->upper->distinct_pathkeys,
        child_total, input_rows, input_width, 0.0, work_mem, -1.0);
    *out_startup = dummy_path.startup_cost;
    *out_total   = dummy_path.total_cost;

}

static Plan *
pg_build_unique(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
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
    return (Plan *) make_unique(child, ctx->upper->distinctClause);

}

void pg_module_distinct_init(void)
{
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_UNIQUE, .name = "Unique",
        .cost_fn = pg_cost_unique,
        .build_plan_fn = pg_build_unique,
    });

	pg_module_distinct_register_rules();
}

/* Rule declarations for distinct module */
/* Auto-generated from rule.c — do not edit by hand */
extern List *pg_rule_distinct_to_unique(PgPlannerCascadesContext *, PgGroupExpr *);

void pg_module_distinct_register_rules(void)
{
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalDistinct->PhysicalUnique", PG_CASCADES_LOGICAL_DISTINCT, PG_CASCADES_PHYSICAL_UNIQUE,
        NULL, pg_rule_distinct_to_unique, 1.0, PG_RULE_BIT_DISTINCT_TO_UNIQUE);

}
