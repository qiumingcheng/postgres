/* modules/project.c — Project operator (cost + build from planbuild.c) */
#include "postgres.h"
#include "optimizer/planmain.h"
#include "optimizer/cascades.h"

static void
pg_cost_project(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
    PgGroupExpr *expr, double input_rows, int input_width,
    Cost child_startup, Cost child_total,
    Cost *out_startup, Cost *out_total)
{
    *out_startup = child_startup;
    *out_total   = child_total;

	pg_module_project_register_rules();
}

static Plan *
pg_build_project(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
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

    /* No agg/group/window: project is transparent */
    if (ctx->upper->numGroupCols == 0 &&
        !ctx->upper->hasAggs &&
        ctx->upper->activeWindows == NIL)
    {
        if (is_projection_capable_plan(child))
            child->targetlist = ctx->upper->sub_tlist;
        return child;
    }

    /* Agg/grouping: wrap with Result */
    return (Plan *) make_result(ctx->root, ctx->upper->sub_tlist, NULL, child);

	pg_module_project_register_rules();
}

void pg_module_project_init(void)
{
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_PROJECT, .name = "Project",
        .cost_fn = pg_cost_project,
        .build_plan_fn = pg_build_project,
    });

	pg_module_project_register_rules();
}

/* Rule declarations for project module */
/* Auto-generated from rule.c — do not edit by hand */
extern List *pg_rule_eliminate_project(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_merge_project_with_child(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_project_to_physical_project(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_prune_empty_union(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_prune_project_columns(PgPlannerCascadesContext *, PgGroupExpr *);
extern PgPattern *g_pat_project_project_leaf;

void pg_module_project_register_rules(void)
{
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "EliminateProject", PG_CASCADES_LOGICAL_PROJECT, 0,
        NULL, pg_rule_eliminate_project, 0.6, PG_RULE_BIT_ELIMINATE_PROJECT);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalProject->PhysicalProject", PG_CASCADES_LOGICAL_PROJECT, PG_CASCADES_PHYSICAL_PROJECT,
        NULL, pg_rule_project_to_physical_project, 1.0, PG_RULE_BIT_PROJECT_TO_PROJECT);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "MergeProjectWithChild", PG_CASCADES_LOGICAL_PROJECT, 0,
        g_pat_project_project_leaf, pg_rule_merge_project_with_child, 0.5, PG_RULE_BIT_MERGE_PROJECT);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "PruneEmptyUnion", PG_CASCADES_LOGICAL_PROJECT, 0,
        NULL, pg_rule_prune_empty_union, 0.8, PG_RULE_BIT_PRUNE_EMPTY_UNION);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "PruneProjectColumns", PG_CASCADES_LOGICAL_PROJECT, 0,
        NULL, pg_rule_prune_project_columns, 0.6, PG_RULE_BIT_PRUNE_PROJ_COLS);

	pg_module_project_register_rules();
}
