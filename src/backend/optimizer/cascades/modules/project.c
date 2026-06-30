/*-------------------------------------------------------------------------
 * project.c
 *    Project operator module for Cascades optimizer
 *
 * Handles logical and physical projection operations (SELECT clause).
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/planmain.h"
#include "optimizer/cascades.h"
#include "core/registry.h"
#include "nodes/primnodes.h"

extern PgPattern *g_pat_leaf1, *g_pat_project_project_leaf;

/* Forward declarations */
static void pg_cost_project_fn(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
                               PgGroupExpr *expr, double input_rows, int input_width,
                               Cost child_startup, Cost child_total,
                               Cost *out_startup, Cost *out_total);
static Plan *pg_build_project_fn(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
                                 PgGroupExpr *expr, PgGroupBestEntry *best,
                                 PgRequiredProperty *required, PgOutputProperty *output);

/* Helper function to check if a plan node is a scan */
static bool
pg_is_scan_node(Plan *plan)
{
    if (plan == NULL)
        return false;

    return (IsA(plan, SeqScan) ||
            IsA(plan, IndexScan) ||
            IsA(plan, IndexOnlyScan) ||
            IsA(plan, BitmapHeapScan));
}

/* Helper function to recursively check if a join tree contains only scans */
static bool
pg_is_simple_join_tree(Plan *plan)
{
    Plan *left, *right;

    if (plan == NULL)
        return false;

    /* If it's a scan, it's simple */
    if (pg_is_scan_node(plan))
        return true;

    /* If it's not a join, it's not simple */
    if (!IsA(plan, NestLoop) && !IsA(plan, HashJoin) && !IsA(plan, MergeJoin))
        return false;

    /* For joins, check both children */
    left = plan->lefttree;
    right = plan->righttree;

    /* Unwrap utility nodes (Materialize, Hash, Sort) */
    if (IsA(right, Material))
        right = right->lefttree;
    if (IsA(right, Hash))
        right = right->lefttree;
    if (IsA(left, Sort))
        left = left->lefttree;
    if (IsA(right, Sort))
        right = right->lefttree;

    /* Recursively check both children */
    return pg_is_simple_join_tree(left) && pg_is_simple_join_tree(right);
}

/* ========================================================================
 * Cost Function
 * ======================================================================== */

static void
pg_cost_project_fn(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
                  PgGroupExpr *expr, double input_rows, int input_width,
                  Cost child_startup, Cost child_total,
                  Cost *out_startup, Cost *out_total)
{
    /* Project has no additional cost - just pass through child costs */
    *out_startup = child_startup;
    *out_total = child_total;
}

/* ========================================================================
 * Build Plan Function
 * ======================================================================== */

static Plan *
pg_build_project_fn(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
                   PgGroupExpr *expr, PgGroupBestEntry *best,
                   PgRequiredProperty *required, PgOutputProperty *output)
{
    Plan              *child;
    PgGroupBestEntry  *child_best = NULL;
    PgOutputProperty   child_out;
    PgMemoGroup       *child_group;
    ListCell          *lc;
    List              *project_tlist;
    bool               is_simple_projection;

    if (expr->inputs == NIL)
        return NULL;

    child_group = (PgMemoGroup *) linitial(expr->inputs);
    if (child_group->best_entries == NIL)
        return NULL;

    /* Two-pass child selection: prefer COMPOSABLE_OP over IMPORTED_PATH */

    /* Pass 1: COMPOSABLE_OP with matching required properties */
    foreach(lc, child_group->best_entries)
    {
        PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
        if (best->child_required_props != NIL &&
            e->expr->mode == PG_PHYS_EXPR_COMPOSABLE_OP &&
            pg_required_property_equal(e->required, pg_safe_linitial_child_req(best)))
        {
            child_best = e;
            break;
        }
    }

    /* Pass 2: any entry with matching required properties */
    if (child_best == NULL)
    {
        foreach(lc, child_group->best_entries)
        {
            PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
            if (best->child_required_props != NIL &&
                pg_required_property_equal(e->required, pg_safe_linitial_child_req(best)))
            {
                child_best = e;
                break;
            }
        }
    }

    /* Fallback: first available entry */
    if (child_best == NULL)
        child_best = (PgGroupBestEntry *) linitial(child_group->best_entries);

    /* Recursively build child plan */
    child = pg_cascades_build_plan_recurse(ctx, child_group,
                                          pg_safe_linitial_child_req(best),
                                          child_best, &child_out);
    if (child == NULL)
        return NULL;

    *output = best->output;

    /* Check if this is a simple projection (all Vars, no expressions) */
    project_tlist = ctx->upper->sub_tlist;
    is_simple_projection = true;

    if (project_tlist != NIL)
    {
        foreach(lc, project_tlist)
        {
            TargetEntry *te = (TargetEntry *) lfirst(lc);
            if (te != NULL && te->expr != NULL && !IsA(te->expr, Var))
            {
                is_simple_projection = false;
                break;
            }
        }
    }

    /*
     * Optimization: For simple projections over joins that contain only scans,
     * we can skip the Result node wrapper.
     *
     * This eliminates the extra Result node for simple join queries like:
     *   SELECT * FROM x, y WHERE x.id = y.id
     *   SELECT * FROM x, y, z WHERE x.id = y.id AND y.id = z.id
     */
    if (is_simple_projection &&
        (IsA(child, NestLoop) || IsA(child, HashJoin) || IsA(child, MergeJoin)))
    {
        /* Check if the entire join tree contains only scan nodes */
        if (pg_is_simple_join_tree(child))
        {
            /* Safe: simple join tree with simple projection - no Result needed */
            CASCADES_DEBUG(ctx->debug,
                "CASCADES: Simple join tree with simple projection - no Result node");
            return child;
        }
    }

use_result:
    /* All other cases: wrap with Result to ensure correct column projection */
    return (Plan *) make_result(ctx->root, project_tlist, NULL, child);
}

/* ========================================================================
 * Implementation Rule: LogicalProject → PhysicalProject
 * ======================================================================== */

List *
pg_rule_project_to_physical_project(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *r = pg_memo_new_group_expr(ctx, PG_CASCADES_PHYSICAL_PROJECT);
    r->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    r->inputs = expr->inputs;
    r->op_private = ctx->upper;
    return list_make1(r);
}

/* ========================================================================
 * Transform Rule: MergeProjectWithChild
 *
 * Pattern: Project(Project(...)) → Project(...)
 * Merges consecutive Project nodes.
 * ======================================================================== */

List *
pg_rule_merge_project_with_child(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *cg;
    PgGroupExpr *in = NULL;
    PgGroupExpr *np;
    ListCell    *lc;

    if (expr->inputs == NIL)
        return NIL;

    cg = (PgMemoGroup *) linitial(expr->inputs);
    if (cg == NULL || cg->logical_exprs == NIL)
        return NIL;

    /* Find child Project */
    foreach(lc, cg->logical_exprs)
    {
        PgGroupExpr *e = (PgGroupExpr *) lfirst(lc);
        if (e->op == PG_CASCADES_LOGICAL_PROJECT)
        {
            in = e;
            break;
        }
    }

    if (in == NULL)
        return NIL;

    /* Create merged Project: outer's tlist, inner's child */
    np = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_PROJECT);
    np->inputs = in->inputs;
    np->op_private = expr->op_private;
    return list_make1(np);
}

/* ========================================================================
 * Transform Rule: EliminateProject
 *
 * Pattern: Project(Project(...)) where both have same projection → Project(...)
 * Eliminates duplicate consecutive Project nodes.
 * ======================================================================== */

List *
pg_rule_eliminate_project(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *child_group;
    PgGroupExpr *child_expr = NULL;
    ListCell    *lc;

    if (expr->inputs == NIL)
        return NIL;

    child_group = (PgMemoGroup *) linitial(expr->inputs);
    if (child_group == NULL || child_group->logical_exprs == NIL)
        return NIL;

    /* Only check if it's another Project (original conservative logic) */
    foreach(lc, child_group->logical_exprs)
    {
        PgGroupExpr *e = (PgGroupExpr *) lfirst(lc);
        if (e->op == PG_CASCADES_LOGICAL_PROJECT)
        {
            child_expr = e;
            /* Eliminate duplicate Project: Project(Project(...)) with same tlist */
            if (expr->op_private == child_expr->op_private)
            {
                PgGroupExpr *ne = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_PROJECT);
                ne->inputs = child_expr->inputs;
                ne->op_private = child_expr->op_private;
                return list_make1(ne);
            }
            break;
        }
    }

    return NIL;
}

/* ========================================================================
 * Transform Rule: PruneEmptyUnion
 *
 * Pattern: Project(Union(...)) where Union has no useful work → Union(...)
 * ======================================================================== */

List *
pg_rule_prune_empty_union(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *cg;
    PgGroupExpr *uo = NULL;
    ListCell    *lc;

    if (expr->inputs == NIL)
        return NIL;

    cg = (PgMemoGroup *) linitial(expr->inputs);

    foreach(lc, cg->logical_exprs)
    {
        PgGroupExpr *e = (PgGroupExpr *) lfirst(lc);
        if (e->op == PG_CASCADES_LOGICAL_UNION)
        {
            uo = e;
            break;
        }
    }

    if (uo == NULL)
        return NIL;

    return list_make1(uo);
}

/* ========================================================================
 * Transform Rule: PruneProjectColumns
 *
 * Collects used columns from projection and propagates to logical property.
 * ======================================================================== */

List *
pg_rule_prune_project_columns(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    List       *tlist = (List *) expr->op_private;
    ListCell   *lc;
    Bitmapset  *n = NULL;

    if (tlist == NIL)
        return NIL;

    /* Collect all Var attribute numbers from projection expressions */
    foreach(lc, tlist)
    {
        TargetEntry *te = (TargetEntry *) lfirst(lc);
        if (te->expr != NULL)
            pg_collect_all_var_attnos((Node *) te->expr, &n);
    }

    if (n != NULL)
    {
        if (expr->owner_group->logical_prop.output_columns != NULL)
            n = bms_union(n, expr->owner_group->logical_prop.output_columns);
        expr->owner_group->logical_prop.output_columns = n;
    }

    return NIL;
}

/* ========================================================================
 * Module Initialization
 * ======================================================================== */

void
pg_module_project_init(void)
{
    /* Register operator vtable */
    pg_registry_register_operator(&(PgOperatorVtable) {
        .op = PG_CASCADES_PHYSICAL_PROJECT,
        .name = "Project",
        .cost_fn = pg_cost_project_fn,
        .build_plan_fn = pg_build_project_fn
    });

    /* Implementation rule */
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
                              "LogicalProject→PhysicalProject",
                              PG_CASCADES_LOGICAL_PROJECT,
                              PG_CASCADES_PHYSICAL_PROJECT,
                              g_pat_leaf1,
                              pg_rule_project_to_physical_project,
                              1.0,
                              PG_RULE_BIT_PROJECT_TO_PROJECT);

    /* Transformation rules */
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
                              "MergeProjectWithChild",
                              PG_CASCADES_LOGICAL_PROJECT,
                              0,
                              g_pat_project_project_leaf,
                              pg_rule_merge_project_with_child,
                              0.5,
                              PG_RULE_BIT_MERGE_PROJECT);

    /* Enhanced eliminate rule - now handles Project(Join) */
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
                              "EliminateProject",
                              PG_CASCADES_LOGICAL_PROJECT,
                              0,
                              NULL,
                              pg_rule_eliminate_project,
                              0.9,  /* High promise - should run early */
                              PG_RULE_BIT_ELIMINATE_PROJECT);

    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
                              "PruneProjectColumns",
                              PG_CASCADES_LOGICAL_PROJECT,
                              0,
                              NULL,
                              pg_rule_prune_project_columns,
                              0.6,
                              PG_RULE_BIT_PRUNE_PROJ_COLS);

    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
                              "PruneEmptyUnion",
                              PG_CASCADES_LOGICAL_PROJECT,
                              0,
                              NULL,
                              pg_rule_prune_empty_union,
                              0.8,
                              PG_RULE_BIT_PRUNE_EMPTY_UNION);
}
