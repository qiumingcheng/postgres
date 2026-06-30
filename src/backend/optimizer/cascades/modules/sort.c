#include "postgres.h"
#include "miscadmin.h"
#include "optimizer/planmain.h"
#include "optimizer/cascades.h"
#include "core/registry.h"
#include "optimizer/cost.h"
extern PgPattern *g_pat_leaf1;

static void
pg_cost_sort_fn(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
                PgGroupExpr *expr, double input_rows, int input_width,
                Cost child_startup, Cost child_total,
                Cost *out_startup, Cost *out_total)
{
    Path            dummy_path;
    PgSortPrivate  *sp = (PgSortPrivate *) expr->op_private;
    double          limit_tuples;

    limit_tuples = (sp && sp->limit_tuples > 0) ? sp->limit_tuples : -1.0;
    MemSet(&dummy_path, 0, sizeof(Path));
    dummy_path.pathtype = T_Sort;
    cost_sort(&dummy_path, ctx->root, sp ? sp->pathkeys : NIL,
              child_total, input_rows, input_width, 0.0, work_mem, limit_tuples);
    *out_startup = dummy_path.startup_cost;
    *out_total   = dummy_path.total_cost;
}

static Plan *
pg_build_sort_fn(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
                 PgGroupExpr *expr, PgGroupBestEntry *best,
                 PgRequiredProperty *required, PgOutputProperty *output)
{
    Plan               *child;
    PgGroupBestEntry   *child_best = NULL;
    PgOutputProperty    child_out;
    PgMemoGroup        *child_group;
    ListCell           *lc;

    if (expr->inputs == NIL)
        return NULL;
    child_group = (PgMemoGroup *) linitial(expr->inputs);
    if (child_group->best_entries == NIL)
        return NULL;

    /* Pass 1: prefer COMPOSABLE_OP child (Project) to ensure
     * column projection matches the SELECT list.  Without this,
     * IMPORTED_PATH Scan may be selected first outputting all
     * table columns → "attribute N has wrong type". */
    foreach(lc, child_group->best_entries)
    {
        PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
        if (best->child_required_props != NIL &&
            e->expr->mode == PG_PHYS_EXPR_COMPOSABLE_OP &&
            pg_required_property_equal(e->required,
                pg_safe_linitial_child_req(best)))
        { child_best = e; break; }
    }
    /* Pass 2: fallback to any matching child (IMPORTED_PATH) */
    if (child_best == NULL)
    {
        foreach(lc, child_group->best_entries)
        {
            PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
            if (best->child_required_props != NIL &&
                pg_required_property_equal(e->required,
                    pg_safe_linitial_child_req(best)))
            { child_best = e; break; }
        }
    }
    if (child_best == NULL)
        return NULL;

    child = pg_cascades_build_plan_recurse(ctx, child_group,
                pg_safe_linitial_child_req(best), child_best, &child_out);
    if (child == NULL)
        return NULL;

    *output = best->output;
    {
        double lt = (ctx->upper->limit_tuples > 0) ? ctx->upper->limit_tuples
                    : required->limit_tuples;
        child = (Plan *) make_sort_from_pathkeys(ctx->root, child,
                    required->pathkeys, lt);
        if (lt > 0)
            child = (Plan *) make_limit(child,
                ctx->root->parse->limitOffset,
                ctx->root->parse->limitCount,
                ctx->upper->offset_est, lt);
    }
    return child;
}

/* === pg_rule_sort_to_physical_sort (moved from rule.c) === */

List *
pg_rule_sort_to_physical_sort(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx, PG_CASCADES_PHYSICAL_SORT);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = expr->inputs;
    result->op_private = ctx->upper;
    return list_make1(result);
}

/* === pg_rule_eliminate_sort_with_constant_key (moved from rule.c) === */

List *
pg_rule_eliminate_sort_with_constant_key(PgPlannerCascadesContext *ctx,
                                          PgGroupExpr *expr)
{
    if (ctx->upper->sort_pathkeys == NIL && ctx->upper->group_pathkeys == NIL)
    {
        PgGroupExpr *new_child = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_PROJECT);
        new_child->inputs = expr->inputs;
        new_child->op_private = ctx->upper->tlist;
        return list_make1(new_child);
    }
    return NIL;
}

/* === pg_rule_prune_sort_columns (moved from rule.c) === */

List *
pg_rule_prune_sort_columns(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    Bitmapset  *needed = NULL;
    ListCell   *lc;

    foreach(lc, ctx->upper->sort_pathkeys)
    {
        PathKey *pk = (PathKey *) lfirst(lc);
        ListCell *ec;
        foreach(ec, pk->pk_eclass->ec_members)
        {
            EquivalenceMember *em = (EquivalenceMember *) lfirst(ec);
            if (IsA(em->em_expr, Var))
            {
                Var *v = (Var *) em->em_expr;
                needed = bms_add_member(needed, v->varattno);
            }
        }
    }
    if (needed != NULL)
    {
        if (expr->owner_group->logical_prop.output_columns != NULL)
            needed = bms_union(needed,
                        expr->owner_group->logical_prop.output_columns);
        expr->owner_group->logical_prop.output_columns = needed;
    }
    return NIL;
}

/* === pg_rule_enforce_sort (moved from rule.c) === */

List *
pg_rule_enforce_sort(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    (void) expr;
    return NIL;
}

void
pg_module_sort_init(void)
{
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_SORT, .name = "Sort",
        .cost_fn = pg_cost_sort_fn, .build_plan_fn = pg_build_sort_fn,
    });
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalSort→PhysicalSort",
        PG_CASCADES_LOGICAL_SORT, PG_CASCADES_PHYSICAL_SORT,
        g_pat_leaf1, pg_rule_sort_to_physical_sort,
        1.0, PG_RULE_BIT_SORT_TO_SORT);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "EliminateSortWithConstKey",
        PG_CASCADES_LOGICAL_SORT, 0, NULL,
        pg_rule_eliminate_sort_with_constant_key,
        0.5, PG_RULE_BIT_ELIM_SORT_CONST_KEY);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "PruneSortColumns",
        PG_CASCADES_LOGICAL_SORT, 0, NULL,
        pg_rule_prune_sort_columns,
        0.5, PG_RULE_BIT_PRUNE_SORT_COLS);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "EnforceSort", 0, PG_CASCADES_PHYSICAL_SORT,
        NULL, pg_rule_enforce_sort,
        0.0, PG_RULE_BIT_ENFORCE_SORT);
}
