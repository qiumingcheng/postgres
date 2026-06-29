#include "postgres.h"
#include "optimizer/planmain.h"
#include "optimizer/cascades.h"
#include "core/registry.h"
#include "optimizer/cost.h"
#include "optimizer/clauses.h"
extern PgPattern *g_pat_leaf1,*g_pat_leaf2,*g_pat_agg_agg_leaf,*g_pat_agg_limit_leaf;
static void pg_cost_agg_fn(PgPlannerCascadesContext *ctx,PgMemoGroup *group,PgGroupExpr *expr,double input_rows,int input_width,Cost child_startup,Cost child_total,Cost *out_startup,Cost *out_total){Path dummy_path;int agg_strategy;agg_strategy=(expr->op==PG_CASCADES_PHYSICAL_HASHAGG)?AGG_HASHED:AGG_SORTED;MemSet(&dummy_path,0,sizeof(Path));dummy_path.pathtype=T_Agg;cost_agg(&dummy_path,ctx->root,agg_strategy,&ctx->upper->agg_costs,ctx->upper->numGroupCols,ctx->upper->dNumGroups,child_startup,child_total,input_rows);*out_startup=dummy_path.startup_cost;*out_total=dummy_path.total_cost;}
static Plan *pg_build_agg_fn(PgPlannerCascadesContext *ctx,PgMemoGroup *group,PgGroupExpr *expr,PgGroupBestEntry *best,PgRequiredProperty *required,PgOutputProperty *output){Plan *child;PgGroupBestEntry *child_best=NULL;PgOutputProperty child_out;PgMemoGroup *child_group;ListCell *lc;if(expr->inputs==NIL)return NULL;child_group=(PgMemoGroup*)linitial(expr->inputs);if(child_group->best_entries==NIL)return NULL;if(expr->op==PG_CASCADES_PHYSICAL_HASHAGG&&best->child_required_props==NIL)return NULL;foreach(lc,child_group->best_entries){PgGroupBestEntry *e=(PgGroupBestEntry*)lfirst(lc);if(best->child_required_props!=NIL&&pg_required_property_equal(e->required,pg_safe_linitial_child_req(best))){child_best=e;break;}}if(child_best==NULL&&list_length(child_group->best_entries)==1)child_best=(PgGroupBestEntry*)linitial(child_group->best_entries);if(child_best==NULL)return NULL;child=pg_cascades_build_plan_recurse(ctx,child_group,pg_safe_linitial_child_req(best),child_best,&child_out);if(child==NULL)return NULL;*output=best->output;if(ctx->upper->hasAggs){AggStrategy strategy;strategy=(expr->op==PG_CASCADES_PHYSICAL_HASHAGG)?((ctx->upper->numGroupCols>0)?AGG_HASHED:AGG_PLAIN):AGG_SORTED;return(Plan*)make_agg(ctx->root,ctx->upper->tlist,(List*)ctx->upper->havingQual,strategy,&ctx->upper->agg_costs,ctx->upper->numGroupCols,ctx->upper->groupColIdx,ctx->upper->groupOperators,(long)ctx->upper->dNumGroups,child);}else{return(Plan*)make_group(ctx->root,ctx->upper->tlist,(List*)ctx->upper->havingQual,ctx->upper->numGroupCols,ctx->upper->groupColIdx,ctx->upper->groupOperators,ctx->upper->dNumGroups,child);}}
static void pg_derive_agg_stats_fn(PgPlannerCascadesContext *ctx,PgGroupExpr *expr,double *out_rows,int *out_width){if(ctx->upper&&ctx->upper->dNumGroups>0)*out_rows=ctx->upper->dNumGroups;else if(list_length(expr->inputs)>=1){PgMemoGroup *child=(PgMemoGroup*)linitial(expr->inputs);*out_rows=(child->rows>0)?child->rows*0.1:100;}if(list_length(expr->inputs)>=1)*out_width=((PgMemoGroup*)linitial(expr->inputs))->width;}

/* === pg_rule_agg_to_hashagg (moved from rule.c) === */

List *
pg_rule_agg_to_hashagg(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_HASHAGG);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = expr->inputs;
    result->op_private = ctx->upper;
    return list_make1(result);
}


/* === pg_rule_agg_to_groupagg (moved from rule.c) === */

List *
pg_rule_agg_to_groupagg(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result;

    /* Only apply GroupAgg when GROUP BY is present */
    if (ctx->upper->groupClause == NIL)
        return NIL;

    result = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_PHYSICAL_GROUPAGG);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = expr->inputs;
    result->op_private = ctx->upper;
    return list_make1(result);
}


/* === pg_rule_merge_two_agg (moved from rule.c) === */

List *
pg_rule_merge_two_agg(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *inner;

    inner = pg_memo_group_first_logical(
        (PgMemoGroup *) linitial(expr->inputs), PG_CASCADES_LOGICAL_AGG);
    if (inner == NULL)
        return NIL;

    /*
     * 第一版简化：仅当内外 Agg 的 op_private 指针相同时合并
     * （在 pg_adapter 构建时，同一查询的 Agg 节点共享 PgCascadesUpperInfo）。
     */
    if (expr->op_private != inner->op_private)
        return NIL;

    /* 跳过内层 Agg，直接引用内层的 child */
    {
        PgGroupExpr *new_agg = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_AGG);
        new_agg->inputs = inner->inputs;
        new_agg->op_private = expr->op_private;
        return list_make1(new_agg);
    }
}


/* === pg_rule_prune_agg_columns (moved from rule.c) === */

List *
pg_rule_prune_agg_columns(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgCascadesUpperInfo *upper = ctx->upper;
    Bitmapset  *needed = NULL;
    ListCell   *lc;

    if (upper == NULL)
        return NIL;

    /* Collect GROUP BY columns */
    if (upper->groupClause != NIL)
    {
        foreach(lc, upper->groupClause)
        {
            SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
            TargetEntry *te = get_sortgroupclause_tle(sgc, upper->tlist);
            if (te != NULL)
                needed = bms_add_member(needed, te->resno);
        }
    }

    /* Collect aggref argument columns from target list */
    foreach(lc, upper->tlist)
    {
        TargetEntry *te = (TargetEntry *) lfirst(lc);
        if (IsA(te->expr, Aggref))
        {
            Aggref *agg = (Aggref *) te->expr;
            ListCell *alc;
            foreach(alc, agg->args)
            {
                Node *arg = (Node *) lfirst(alc);
                if (IsA(arg, Var))
                    needed = bms_add_member(needed, ((Var *) arg)->varattno);
            }
        }
    }

    if (needed != NULL)
    {
        /* Prune: union with existing output_columns so we don't lose parent needs */
        if (expr->owner_group->logical_prop.output_columns != NULL)
            needed = bms_union(needed,
                        expr->owner_group->logical_prop.output_columns);
        expr->owner_group->logical_prop.output_columns = needed;
    }
    return NIL;
}


/* === pg_collect_all_var_attnos (moved from rule.c) === */

bool
pg_collect_var_attnos_walker(Node *node, void *context)
{
    Bitmapset **needed = (Bitmapset **) context;
    if (node == NULL)
        return false;
    if (IsA(node, Var))
    {
        Var *var = (Var *) node;
        *needed = bms_add_member(*needed, var->varattno);
        return false;
    }
    return expression_tree_walker(node, pg_collect_var_attnos_walker, context);
}

void
pg_collect_all_var_attnos(Node *expr, Bitmapset **needed)
{
    pg_collect_var_attnos_walker(expr, needed);
}


/* === pg_rule_pushdown_agg_limit (moved from rule.c) === */

List *
pg_rule_pushdown_agg_limit(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *limit_expr;

    limit_expr = pg_memo_group_first_logical(
        (PgMemoGroup *) linitial(expr->inputs), PG_CASCADES_LOGICAL_LIMIT);
    if (limit_expr == NULL)
        return NIL;

    /* Build: Limit(Agg(A)) */
    {
        PgGroupExpr *new_agg = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_AGG);
        new_agg->inputs = limit_expr->inputs;
        new_agg->op_private = expr->op_private;

        PgGroupExpr *new_limit = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_LIMIT);
        new_limit->op_private = limit_expr->op_private;
        return list_make1(new_limit);
    }
}


/* === pg_rule_eliminate_agg (moved from rule.c) === */

List *
pg_rule_eliminate_agg(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *child_group;

    /* Only eliminate if there's no aggregation and no GROUP BY */
    if (ctx->root->parse->hasAggs || ctx->root->parse->groupClause != NIL)
        return NIL;

    child_group = (PgMemoGroup *) linitial(expr->inputs);
    if (child_group == NULL || child_group == expr->owner_group)
        return NIL;

    /* Merge child group's expressions into the parent group */
    pg_memo_merge_group(ctx, expr->owner_group, child_group);

    /*
     * Phase 6: Remove the eliminated expression from the target group.
     * See pg_rule_eliminate_limit for rationale.
     */
    expr->owner_group->logical_exprs = list_delete_ptr(
        expr->owner_group->logical_exprs, expr);

    /* Return NIL — no new expressions to insert */
    return NIL;
}

void pg_module_agg_init(void){pg_registry_register_operator(&(PgOperatorVtable){.op=PG_CASCADES_PHYSICAL_HASHAGG,.name="HashAgg",.cost_fn=pg_cost_agg_fn,.build_plan_fn=pg_build_agg_fn,.derive_stats_fn=pg_derive_agg_stats_fn});pg_registry_register_operator(&(PgOperatorVtable){.op=PG_CASCADES_PHYSICAL_GROUPAGG,.name="GroupAgg",.cost_fn=pg_cost_agg_fn,.build_plan_fn=pg_build_agg_fn,.derive_stats_fn=pg_derive_agg_stats_fn});pg_registry_register_rule(PG_RULE_MEMO_IMPL,"LogicalAgg→PhysicalHashAgg",PG_CASCADES_LOGICAL_AGG,PG_CASCADES_PHYSICAL_HASHAGG,g_pat_leaf2,pg_rule_agg_to_hashagg,0.8,PG_RULE_BIT_AGG_TO_HASHAGG);pg_registry_register_rule(PG_RULE_MEMO_IMPL,"LogicalAgg→PhysicalGroupAgg",PG_CASCADES_LOGICAL_AGG,PG_CASCADES_PHYSICAL_GROUPAGG,NULL,pg_rule_agg_to_groupagg,0.7,PG_RULE_BIT_AGG_TO_GROUPAGG);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"MergeTwoAgg",PG_CASCADES_LOGICAL_AGG,0,g_pat_agg_agg_leaf,pg_rule_merge_two_agg,0.5,PG_RULE_BIT_MERGE_TWO_AGG);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"PushDownAggLimit",PG_CASCADES_LOGICAL_AGG,0,g_pat_agg_limit_leaf,pg_rule_pushdown_agg_limit,0.4,PG_RULE_BIT_PUSHDOWN_AGG_LIMIT);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"PruneAggColumns",PG_CASCADES_LOGICAL_AGG,0,NULL,pg_rule_prune_agg_columns,0.5,PG_RULE_BIT_PRUNE_AGG_COLS);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"EliminateAgg",PG_CASCADES_LOGICAL_AGG,0,NULL,pg_rule_eliminate_agg,0.6,PG_RULE_BIT_ELIMINATE_AGG);}
