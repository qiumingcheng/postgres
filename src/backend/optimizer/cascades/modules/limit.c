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
}
