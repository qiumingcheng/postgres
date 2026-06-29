#include "postgres.h"
#include "optimizer/cascades.h"
#include "core/registry.h"
extern List *pg_rule_pushdown_predicate_scan(PgPlannerCascadesContext *, PgGroupExpr *);
extern PgPattern *g_pat_filter_join_leaf_leaf;
extern PgPattern *g_pat_filter_project_leaf;

/* === pg_rule_pushdown_predicate_project (moved from rule.c) === */

List *
pg_rule_pushdown_predicate_project(PgPlannerCascadesContext *ctx,
                                    PgGroupExpr *expr)
{
    PgGroupExpr *proj_expr;
    List *filter_quals;
    PgGroupExpr *new_filter, *new_proj;

    proj_expr = pg_memo_group_first_logical(
        (PgMemoGroup *) linitial(expr->inputs), PG_CASCADES_LOGICAL_PROJECT);
    if (proj_expr == NULL)
        return NIL;

    filter_quals = (List *) expr->op_private;
    if (filter_quals == NIL)
        return NIL;

    /* Check volatile — don't move volatile functions */
    {
        ListCell *lc;
        foreach(lc, filter_quals)
        {
            RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
            if (contain_volatile_functions((Node *) ri->clause))
                return NIL;
        }
    }

    /* Build: Project(Filter(A)) where Filter wraps Project's child */
    new_filter = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_FILTER);
    new_filter->inputs = proj_expr->inputs;  /* Project's child */
    new_filter->op_private = filter_quals;

    new_proj = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_PROJECT);
    new_proj->op_private = proj_expr->op_private;  /* tlist */

    return list_make1(new_proj);  /* new_proj's input = new_filter's group */
}


/* === pg_rule_pushdown_predicate_agg (moved from rule.c) === */

List *
pg_rule_pushdown_predicate_agg(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *agg_expr;
    List       *filter_quals;

    agg_expr = pg_memo_group_first_logical(
        (PgMemoGroup *) linitial(expr->inputs), PG_CASCADES_LOGICAL_AGG);
    if (agg_expr == NULL)
        return NIL;

    filter_quals = (List *) expr->op_private;
    if (filter_quals == NIL)
        return NIL;

    /* Check volatile */
    {
        ListCell *lc;
        foreach(lc, filter_quals)
        {
            RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
            if (contain_volatile_functions((Node *) ri->clause))
                return NIL;
        }
    }

    /* Build: Agg(Filter(A)) — move filter below Agg */
    {
        PgGroupExpr *new_filter = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_FILTER);
        new_filter->inputs = agg_expr->inputs;
        new_filter->op_private = filter_quals;

        PgGroupExpr *new_agg = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_AGG);
        new_agg->op_private = agg_expr->op_private;
        return list_make1(new_agg);
    }
}


/* === pg_rule_pushdown_predicate_join (moved from rule.c) === */

List *
pg_rule_pushdown_predicate_join(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *join_expr;
    PgJoinPrivate *join_priv;
    List       *filter_quals;
    PgMemoGroup *outer_grp, *inner_grp;
    Relids      outer_relids, inner_relids;
    List       *outer_quals = NIL;
    List       *inner_quals = NIL;
    ListCell   *lc;
    PgGroupExpr *new_outer, *new_inner, *new_join;

    join_expr = pg_memo_group_first_logical(
        (PgMemoGroup *) linitial(expr->inputs), PG_CASCADES_LOGICAL_JOIN);
    if (join_expr == NULL)
        return NIL;

    join_priv = (PgJoinPrivate *) join_expr->op_private;
    if (join_priv == NULL || join_priv->jointype != JOIN_INNER)
        return NIL;  /* first version: INNER JOIN only */

    filter_quals = (List *) expr->op_private;
    if (filter_quals == NIL)
        return NIL;

    if (list_length(join_expr->inputs) != 2)
        return NIL;
    outer_grp = (PgMemoGroup *) linitial(join_expr->inputs);
    inner_grp = (PgMemoGroup *) lsecond(join_expr->inputs);

    outer_relids = pg_cascades_group_relids(ctx, outer_grp);
    inner_relids = pg_cascades_group_relids(ctx, inner_grp);

    foreach(lc, filter_quals)
    {
        RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

        if (contain_volatile_functions((Node *) ri->clause))
            continue;

        /* Push to outer if qual only references outer relids */
        if (bms_is_subset(ri->clause_relids, outer_relids))
            outer_quals = lappend(outer_quals, ri);

        /* Push to inner if qual only references inner relids */
        if (bms_is_subset(ri->clause_relids, inner_relids))
            inner_quals = lappend(inner_quals, ri);
    }

    if (outer_quals == NIL && inner_quals == NIL)
        return NIL;

    /* Build Filter wrappers for sides with pushed quals */
    new_outer = NULL;
    new_inner = NULL;

    if (outer_quals != NIL)
    {
        new_outer = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_FILTER);
        new_outer->inputs = list_make1(outer_grp);
        new_outer->op_private = outer_quals;
    }

    if (inner_quals != NIL)
    {
        new_inner = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_FILTER);
        new_inner->inputs = list_make1(inner_grp);
        new_inner->op_private = inner_quals;
    }

    /* Rebuild Join */
    new_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    new_join->op_private = join_priv;

    return list_make1(new_join);
}

void pg_module_filter_init(void) {
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"PushDownPredicateScan",PG_CASCADES_LOGICAL_FILTER,0,NULL,pg_rule_pushdown_predicate_scan,0.6,PG_RULE_BIT_PUSHDOWN_PRED_SCAN);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"PushDownPredicateJoin",PG_CASCADES_LOGICAL_FILTER,0,g_pat_filter_join_leaf_leaf,pg_rule_pushdown_predicate_join,0.4,PG_RULE_BIT_PUSHDOWN_PRED_JOIN);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"PushDownPredicateProject",PG_CASCADES_LOGICAL_FILTER,0,g_pat_filter_project_leaf,pg_rule_pushdown_predicate_project,0.5,PG_RULE_BIT_PUSHDOWN_PRED_PROJ);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"PushDownPredicateAgg",PG_CASCADES_LOGICAL_FILTER,0,NULL,pg_rule_pushdown_predicate_agg,0.3,PG_RULE_BIT_PUSHDOWN_PRED_AGG);
}
