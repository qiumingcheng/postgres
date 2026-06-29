/* modules/agg.c — HashAgg / GroupAgg (cost + build + stats) */
#include "postgres.h"
#include "optimizer/planmain.h"
#include "optimizer/cascades.h"
#include "optimizer/cost.h"

/* ── Cost (from task.c switch) ── */
static void
pg_cost_agg(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
            PgGroupExpr *expr, double input_rows, int input_width,
            Cost child_startup, Cost child_total,
            Cost *out_startup, Cost *out_total)
{
    Path dummy_path;
    int  agg_strategy = (expr->op == PG_CASCADES_PHYSICAL_HASHAGG)
                        ? AGG_HASHED : AGG_SORTED;
    MemSet(&dummy_path, 0, sizeof(Path));
    dummy_path.pathtype = T_Agg;
    cost_agg(&dummy_path, ctx->root, agg_strategy,
             &ctx->upper->agg_costs, ctx->upper->numGroupCols,
             ctx->upper->dNumGroups, child_startup, child_total, input_rows);
    *out_startup = dummy_path.startup_cost;
    *out_total   = dummy_path.total_cost;

	pg_module_agg_register_rules();
}

/* ── Build Plan (from planbuild.c switch, exact copy) ── */
static Plan *
pg_build_agg(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
             PgGroupExpr *expr, PgGroupBestEntry *best,
             PgRequiredProperty *required, PgOutputProperty *output)
{
    Plan *child;
    PgGroupBestEntry *child_best = NULL;
    PgOutputProperty child_out;
    PgMemoGroup *child_group;
    ListCell *lc;
    Plan *result = NULL;

    if (expr->inputs == NIL) return NULL;
    child_group = (PgMemoGroup *) linitial(expr->inputs);
    if (child_group->best_entries == NIL) return NULL;

    if (best->child_required_props == NIL && expr->op == PG_CASCADES_PHYSICAL_HASHAGG)
        return NULL;

    foreach(lc, child_group->best_entries)
    {
        PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
        if (best->child_required_props != NIL &&
            pg_required_property_equal(e->required,
                pg_safe_linitial_child_req(best)))
        { child_best = e; break; }
    }
    if (child_best == NULL)
    {
        if (list_length(child_group->best_entries) == 1)
            child_best = (PgGroupBestEntry *) linitial(child_group->best_entries);
    }
    if (child_best == NULL) return NULL;

    child = pg_cascades_build_plan_recurse(ctx, child_group,
        pg_safe_linitial_child_req(best), child_best, &child_out);
    if (child == NULL) return NULL;

    if (ctx->upper->hasAggs)
    {
        AggStrategy agg_strategy;
        if (expr->op == PG_CASCADES_PHYSICAL_HASHAGG)
        {
            agg_strategy = (ctx->upper->numGroupCols > 0)
                           ? AGG_HASHED : AGG_PLAIN;
        }
        else
            agg_strategy = AGG_SORTED;

        result = (Plan *) make_agg(ctx->root,
            ctx->upper->tlist, (List *) ctx->upper->havingQual,
            agg_strategy, &ctx->upper->agg_costs,
            ctx->upper->numGroupCols, ctx->upper->groupColIdx,
            ctx->upper->groupOperators, (long) ctx->upper->dNumGroups, child);
    }
    else
    {
        result = (Plan *) make_group(ctx->root,
            ctx->upper->tlist, (List *) ctx->upper->havingQual,
            ctx->upper->numGroupCols, ctx->upper->groupColIdx,
            ctx->upper->groupOperators, ctx->upper->dNumGroups, child);
    }

    *output = best->output;
    return result;

	pg_module_agg_register_rules();
}

/* ── Stats (from memo.c switch) ── */
static void
pg_derive_agg_stats(PgPlannerCascadesContext *ctx, PgGroupExpr *expr,
                    double *out_rows, int *out_width)
{
    if (ctx->upper && ctx->upper->dNumGroups > 0)
        *out_rows = ctx->upper->dNumGroups;
    else if (list_length(expr->inputs) >= 1)
    {
        PgMemoGroup *child = (PgMemoGroup *) linitial(expr->inputs);
        *out_rows = (child->rows > 0) ? child->rows * 0.1 : 100;
    }
    if (list_length(expr->inputs) >= 1)
        *out_width = ((PgMemoGroup *) linitial(expr->inputs))->width;

	pg_module_agg_register_rules();
}

/* ── init ── */
void pg_module_agg_init(void)
{
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_HASHAGG, .name = "HashAgg",
        .cost_fn = pg_cost_agg,
        .build_plan_fn = pg_build_agg,
        .derive_stats_fn = pg_derive_agg_stats,
    });
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_GROUPAGG, .name = "GroupAgg",
        .cost_fn = pg_cost_agg,
        .build_plan_fn = pg_build_agg,
        .derive_stats_fn = pg_derive_agg_stats,
    });

	pg_module_agg_register_rules();
}

/* Rule declarations for agg module */
/* Auto-generated from rule.c — do not edit by hand */
extern List *pg_rule_agg_to_groupagg(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_agg_to_hashagg(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_eliminate_agg(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_merge_two_agg(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_prune_agg_columns(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_pushdown_agg_limit(PgPlannerCascadesContext *, PgGroupExpr *);
extern PgPattern *g_pat_agg_agg_leaf;
extern PgPattern *g_pat_agg_limit_leaf;

void pg_module_agg_register_rules(void)
{
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "EliminateAgg", PG_CASCADES_LOGICAL_AGG, 0,
        NULL, pg_rule_eliminate_agg, 0.6, PG_RULE_BIT_ELIMINATE_AGG);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalAgg->PhysicalGroupAgg", PG_CASCADES_LOGICAL_AGG, PG_CASCADES_PHYSICAL_GROUPAGG,
        NULL, pg_rule_agg_to_groupagg, 0.7, PG_RULE_BIT_AGG_TO_GROUPAGG);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalAgg->PhysicalHashAgg", PG_CASCADES_LOGICAL_AGG, PG_CASCADES_PHYSICAL_HASHAGG,
        NULL, pg_rule_agg_to_hashagg, 0.8, PG_RULE_BIT_AGG_TO_HASHAGG);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "MergeTwoAgg", PG_CASCADES_LOGICAL_AGG, 0,
        g_pat_agg_agg_leaf, pg_rule_merge_two_agg, 0.5, PG_RULE_BIT_MERGE_TWO_AGG);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "PruneAggColumns", PG_CASCADES_LOGICAL_AGG, 0,
        NULL, pg_rule_prune_agg_columns, 0.5, PG_RULE_BIT_PRUNE_AGG_COLS);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "PushDownAggLimit", PG_CASCADES_LOGICAL_AGG, 0,
        g_pat_agg_limit_leaf, pg_rule_pushdown_agg_limit, 0.4, PG_RULE_BIT_PUSHDOWN_AGG_LIMIT);

	pg_module_agg_register_rules();
}
