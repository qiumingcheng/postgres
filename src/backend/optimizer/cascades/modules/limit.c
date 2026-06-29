#include "postgres.h"
#include "optimizer/planmain.h"
#include "optimizer/cascades.h"
#include "core/registry.h"
extern PgPattern *g_pat_leaf1,*g_pat_limit_sort_leaf,*g_pat_limit_join_leaf_leaf;
static void pg_cost_limit_fn(PgPlannerCascadesContext *ctx,PgMemoGroup *group,PgGroupExpr *expr,double input_rows,int input_width,Cost child_startup,Cost child_total,Cost *out_startup,Cost *out_total){double frac=1.0;if(ctx->upper->limit_tuples>0&&ctx->upper->limit_tuples<input_rows)frac=ctx->upper->limit_tuples/input_rows;*out_startup=child_startup;*out_total=child_startup+(child_total-child_startup)*frac;}
static Plan *pg_build_limit_fn(PgPlannerCascadesContext *ctx,PgMemoGroup *group,PgGroupExpr *expr,PgGroupBestEntry *best,PgRequiredProperty *required,PgOutputProperty *output){Plan*child;PgGroupBestEntry*child_best=NULL;PgOutputProperty child_out;PgMemoGroup*child_group;ListCell*lc;if(expr->inputs==NIL)return NULL;child_group=(PgMemoGroup*)linitial(expr->inputs);if(child_group->best_entries==NIL)return NULL;foreach(lc,child_group->best_entries){PgGroupBestEntry*e=(PgGroupBestEntry*)lfirst(lc);if(best->child_required_props!=NIL&&pg_required_property_equal(e->required,pg_safe_linitial_child_req(best))){child_best=e;break;}}if(child_best==NULL)return NULL;child=pg_cascades_build_plan_recurse(ctx,child_group,pg_safe_linitial_child_req(best),child_best,&child_out);if(child==NULL)return NULL;*output=best->output;return(Plan*)make_limit(child,ctx->root->parse->limitOffset,ctx->root->parse->limitCount,ctx->upper->offset_est,ctx->upper->count_est);}

/* === pg_rule_limit_to_physical_limit (moved from rule.c) === */

List *
pg_rule_limit_to_physical_limit(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_LIMIT);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = expr->inputs;
    result->op_private = ctx->upper;
    return list_make1(result);
}


/* === pg_rule_merge_limit_with_sort (moved from rule.c) === */

List *
pg_rule_merge_limit_with_sort(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *sort_expr;
    PgSortPrivate *sort_priv;
    PgGroupExpr *new_sort;
    double limit_tuples;

    sort_expr = pg_memo_group_first_logical(
        (PgMemoGroup *) linitial(expr->inputs), PG_CASCADES_LOGICAL_SORT);
    if (sort_expr == NULL)
        return NIL;

    limit_tuples = ctx->upper->limit_tuples;
    if (limit_tuples <= 0)
        return NIL;  /* 无有效 LIMIT */

    /* 创建新的 LogicalSort，将 limit_tuples 注入 PgSortPrivate */
    new_sort = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_SORT);
    new_sort->inputs = sort_expr->inputs;  /* child 透传 */

    sort_priv = (PgSortPrivate *) palloc(sizeof(PgSortPrivate));
    sort_priv->pathkeys = (List *) sort_expr->op_private;
    sort_priv->limit_tuples = limit_tuples;
    new_sort->op_private = sort_priv;

    return list_make1(new_sort);
}


/* === pg_rule_pushdown_limit_join (moved from rule.c) === */

List *
pg_rule_pushdown_limit_join(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *join_expr;
    PgJoinPrivate *join_priv;
    PgGroupExpr *new_limit_outer, *new_limit_inner;
    PgGroupExpr *new_join;

    join_expr = pg_memo_group_first_logical(
        (PgMemoGroup *) linitial(expr->inputs), PG_CASCADES_LOGICAL_JOIN);
    if (join_expr == NULL)
        return NIL;

    join_priv = (PgJoinPrivate *) join_expr->op_private;
    if (join_priv == NULL || join_priv->jointype != JOIN_INNER)
        return NIL;

    /* Only push if we have a meaningful limit */
    if (ctx->upper->limit_tuples <= 0)
        return NIL;

    /* Create LogicalLimit wrappers for both children */
    new_limit_outer = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_LIMIT);
    new_limit_outer->inputs = list_make1(linitial(join_expr->inputs));
    new_limit_outer->op_private = ctx->upper;

    new_limit_inner = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_LIMIT);
    new_limit_inner->inputs = list_make1(lsecond(join_expr->inputs));
    new_limit_inner->op_private = ctx->upper;

    /* Rebuild Join with original children (Limit pushdown is semantic:
     * the Limit wrappers are inserted into children's groups,
     * and cost-based search will use them when beneficial.) */
    new_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    new_join->inputs = list_make2(linitial(join_expr->inputs),
                                   lsecond(join_expr->inputs));
    new_join->op_private = join_priv;

    /*
     * Return all three: Limit wrappers get inserted into the group first,
     * then the new_join references the same child groups.  The engine
     * processes them sequentially so Limit wrappers exist before new_join
     * is optimized.
     */
    return list_make3(new_limit_outer, new_limit_inner, new_join);
}


/* === pg_rule_eliminate_limit (moved from rule.c) === */

List *
pg_rule_eliminate_limit(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    Query *parse = ctx->root->parse;
    PgMemoGroup *child_group;

    /* Only eliminate if there's actually no LIMIT/OFFSET */
    if (parse->limitCount != NULL || parse->limitOffset != NULL)
        return NIL;

    child_group = (PgMemoGroup *) linitial(expr->inputs);
    if (child_group == NULL || child_group == expr->owner_group)
        return NIL;

    /* Merge child group's expressions into the parent group */
    pg_memo_merge_group(ctx, expr->owner_group, child_group);

    /*
     * Phase 6: After merging, remove the eliminated expression (LogicalLimit)
     * from the target group.  Its inputs still point to the now-empty source
     * group, and keeping it would cause planbuild failures when the task
     * scheduler tries to optimize a child with no expressions.
     */
    expr->owner_group->logical_exprs = list_delete_ptr(
        expr->owner_group->logical_exprs, expr);

    /* Return NIL — no new expressions to insert; group merging was a side effect */
    return NIL;
}


/* === pg_rule_merge_limit_with_child_limit (moved from rule.c) === */

List *
pg_rule_merge_limit_with_child_limit(PgPlannerCascadesContext *ctx,
                                      PgGroupExpr *expr)
{
    PgMemoGroup *child_group;
    PgGroupExpr *inner_limit = NULL;
    ListCell *lc;

    child_group = (PgMemoGroup *) linitial(expr->inputs);

    /* Find a LogicalLimit in the child group */
    foreach(lc, child_group->logical_exprs)
    {
        PgGroupExpr *e = (PgGroupExpr *) lfirst(lc);
        if (e->op == PG_CASCADES_LOGICAL_LIMIT)
        {
            inner_limit = e;
            break;
        }
    }

    if (inner_limit == NULL)
        return NIL;

    /*
     * Merge: create a new Limit that takes the stricter of the two.
     * Since both Limits come from the same query's LIMIT clause
     * (or from rule applications), they should have the same limit
     * values.  We just pass through to the inner Limit's child.
     */
    {
        PgGroupExpr *new_limit = pg_memo_new_group_expr(ctx,
                                        PG_CASCADES_LOGICAL_LIMIT);
        new_limit->inputs = inner_limit->inputs;  /* skip inner Limit */
        new_limit->op_private = expr->op_private; /* keep outer info */
        return list_make1(new_limit);
    }
}

void pg_module_limit_init(void){pg_registry_register_operator(&(PgOperatorVtable){.op=PG_CASCADES_PHYSICAL_LIMIT,.name="Limit",.cost_fn=pg_cost_limit_fn,.build_plan_fn=pg_build_limit_fn});pg_registry_register_rule(PG_RULE_MEMO_IMPL,"LogicalLimit→PhysicalLimit",PG_CASCADES_LOGICAL_LIMIT,PG_CASCADES_PHYSICAL_LIMIT,g_pat_leaf1,pg_rule_limit_to_physical_limit,1.0,PG_RULE_BIT_LIMIT_TO_LIMIT);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"MergeLimitWithSort",PG_CASCADES_LOGICAL_LIMIT,0,g_pat_limit_sort_leaf,pg_rule_merge_limit_with_sort,0.7,PG_RULE_BIT_MERGE_LIMIT_SORT);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"PushDownLimitJoin",PG_CASCADES_LOGICAL_LIMIT,0,g_pat_limit_join_leaf_leaf,pg_rule_pushdown_limit_join,0.4,PG_RULE_BIT_PUSHDOWN_LIMIT_JOIN);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"EliminateLimit",PG_CASCADES_LOGICAL_LIMIT,0,NULL,pg_rule_eliminate_limit,0.6,PG_RULE_BIT_ELIMINATE_LIMIT);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"MergeLimitWithChildLimit",PG_CASCADES_LOGICAL_LIMIT,0,NULL,pg_rule_merge_limit_with_child_limit,0.45,PG_RULE_BIT_MERGE_LIMIT_CHILD_LIMIT);}
