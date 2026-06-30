#include "postgres.h"
#include "optimizer/planmain.h"
#include "optimizer/cascades.h"
#include "core/registry.h"
#include "optimizer/cost.h"
#include "optimizer/clauses.h"
extern PgPattern *g_pat_leaf1,*g_pat_leaf2,*g_pat_agg_agg_leaf,*g_pat_agg_limit_leaf;

static void
pg_cost_agg_fn(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
               PgGroupExpr *expr, double input_rows, int input_width,
               Cost child_startup, Cost child_total,
               Cost *out_startup, Cost *out_total)
{
    Path dummy_path; int agg_strategy;
    agg_strategy = (expr->op == PG_CASCADES_PHYSICAL_HASHAGG) ? AGG_HASHED : AGG_SORTED;
    MemSet(&dummy_path, 0, sizeof(Path)); dummy_path.pathtype = T_Agg;
    cost_agg(&dummy_path, ctx->root, agg_strategy, &ctx->upper->agg_costs,
             ctx->upper->numGroupCols, ctx->upper->dNumGroups,
             child_startup, child_total, input_rows);
    *out_startup = dummy_path.startup_cost; *out_total = dummy_path.total_cost;
}

static Plan *
pg_build_agg_fn(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
                PgGroupExpr *expr, PgGroupBestEntry *best,
                PgRequiredProperty *required, PgOutputProperty *output)
{
    Plan *child; PgGroupBestEntry *child_best = NULL; PgOutputProperty child_out;
    PgMemoGroup *child_group; ListCell *lc;

    if (expr->inputs == NIL) return NULL;
    child_group = (PgMemoGroup *) linitial(expr->inputs);
    if (child_group->best_entries == NIL) return NULL;
    if (expr->op == PG_CASCADES_PHYSICAL_HASHAGG && best->child_required_props == NIL)
        return NULL;

    /* Pass 1: prefer COMPOSABLE_OP */
    foreach(lc, child_group->best_entries) {
        PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
        if (best->child_required_props != NIL &&
            e->expr->mode == PG_PHYS_EXPR_COMPOSABLE_OP &&
            pg_required_property_equal(e->required, pg_safe_linitial_child_req(best)))
        { child_best = e; break; }
    }
    if (child_best == NULL) {
        foreach(lc, child_group->best_entries) {
            PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
            if (best->child_required_props != NIL &&
                pg_required_property_equal(e->required, pg_safe_linitial_child_req(best)))
            { child_best = e; break; }
        }
    }
    if (child_best == NULL && list_length(child_group->best_entries) == 1)
        child_best = (PgGroupBestEntry *) linitial(child_group->best_entries);
    if (child_best == NULL) return NULL;

    child = pg_cascades_build_plan_recurse(ctx, child_group,
                pg_safe_linitial_child_req(best), child_best, &child_out);
    if (child == NULL) return NULL;
    *output = best->output;
    if (ctx->upper->hasAggs) {
        AggStrategy s = (expr->op == PG_CASCADES_PHYSICAL_HASHAGG)
            ? ((ctx->upper->numGroupCols > 0) ? AGG_HASHED : AGG_PLAIN) : AGG_SORTED;
        return (Plan *) make_agg(ctx->root, ctx->upper->tlist,
            (List *) ctx->upper->havingQual, s, &ctx->upper->agg_costs,
            ctx->upper->numGroupCols, ctx->upper->groupColIdx,
            ctx->upper->groupOperators, (long) ctx->upper->dNumGroups, child);
    } else {
        return (Plan *) make_group(ctx->root, ctx->upper->tlist,
            (List *) ctx->upper->havingQual, ctx->upper->numGroupCols,
            ctx->upper->groupColIdx, ctx->upper->groupOperators,
            ctx->upper->dNumGroups, child);
    }
}

static void
pg_derive_agg_stats_fn(PgPlannerCascadesContext *ctx, PgGroupExpr *expr,
                       double *out_rows, int *out_width)
{
    if (ctx->upper && ctx->upper->dNumGroups > 0) *out_rows = ctx->upper->dNumGroups;
    else if (list_length(expr->inputs) >= 1) {
        PgMemoGroup *child = (PgMemoGroup *) linitial(expr->inputs);
        *out_rows = (child->rows > 0) ? child->rows * 0.1 : 100;
    }
    if (list_length(expr->inputs) >= 1)
        *out_width = ((PgMemoGroup *) linitial(expr->inputs))->width;
}

/* transform functions kept compressed */

List *pg_rule_agg_to_hashagg(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{ PgGroupExpr *r=pg_memo_new_group_expr(ctx,PG_CASCADES_PHYSICAL_HASHAGG); r->mode=PG_PHYS_EXPR_COMPOSABLE_OP; r->inputs=expr->inputs; r->op_private=ctx->upper; return list_make1(r); }

List *pg_rule_agg_to_groupagg(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{ if(ctx->upper->groupClause==NIL)return NIL; PgGroupExpr *r=pg_memo_new_group_expr(ctx,PG_CASCADES_PHYSICAL_GROUPAGG); r->mode=PG_PHYS_EXPR_COMPOSABLE_OP; r->inputs=expr->inputs; r->op_private=ctx->upper; return list_make1(r); }

List *pg_rule_merge_two_agg(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{ PgGroupExpr *inner=pg_memo_group_first_logical((PgMemoGroup*)linitial(expr->inputs),PG_CASCADES_LOGICAL_AGG); if(inner==NULL||expr->op_private!=inner->op_private)return NIL; PgGroupExpr *r=pg_memo_new_group_expr(ctx,PG_CASCADES_LOGICAL_AGG); r->inputs=inner->inputs; r->op_private=expr->op_private; return list_make1(r); }

List *pg_rule_prune_agg_columns(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{ PgCascadesUpperInfo *u=ctx->upper; Bitmapset *n=NULL; ListCell *lc; if(u==NULL)return NIL; if(u->groupClause!=NIL){foreach(lc,u->groupClause){SortGroupClause*s=(SortGroupClause*)lfirst(lc); TargetEntry*te=get_sortgroupclause_tle(s,u->tlist); if(te)n=bms_add_member(n,te->resno);}} foreach(lc,u->tlist){TargetEntry*te=(TargetEntry*)lfirst(lc); if(IsA(te->expr,Aggref)){Aggref*a=(Aggref*)te->expr; ListCell*ac; foreach(ac,a->args){Node*arg=(Node*)lfirst(ac); if(IsA(arg,Var))n=bms_add_member(n,((Var*)arg)->varattno);}}} if(n!=NULL){if(expr->owner_group->logical_prop.output_columns!=NULL)n=bms_union(n,expr->owner_group->logical_prop.output_columns); expr->owner_group->logical_prop.output_columns=n;} return NIL; }

bool pg_collect_var_attnos_walker(Node *node, void *context)
{ Bitmapset **n=(Bitmapset**)context; if(node==NULL)return false; if(IsA(node,Var)){*n=bms_add_member(*n,((Var*)node)->varattno); return false;} return expression_tree_walker(node,pg_collect_var_attnos_walker,context); }
void pg_collect_all_var_attnos(Node *expr, Bitmapset **needed){ pg_collect_var_attnos_walker(expr,needed); }

List *pg_rule_pushdown_agg_limit(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{ PgGroupExpr *le=pg_memo_group_first_logical((PgMemoGroup*)linitial(expr->inputs),PG_CASCADES_LOGICAL_LIMIT); if(le==NULL)return NIL; PgGroupExpr *na=pg_memo_new_group_expr(ctx,PG_CASCADES_LOGICAL_AGG); na->inputs=le->inputs; na->op_private=expr->op_private; PgGroupExpr *nl=pg_memo_new_group_expr(ctx,PG_CASCADES_LOGICAL_LIMIT); nl->op_private=le->op_private; nl->inputs=list_make1(na); return list_make1(nl); }

List *pg_rule_eliminate_agg(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{ PgCascadesUpperInfo *u=(PgCascadesUpperInfo*)expr->op_private; if(u==NULL||u->hasAggs||u->groupClause!=NIL)return NIL; PgGroupExpr *r=pg_memo_new_group_expr(ctx,PG_CASCADES_LOGICAL_PROJECT); r->inputs=expr->inputs; r->op_private=u->tlist; return list_make1(r); }

void pg_module_agg_init(void){pg_registry_register_operator(&(PgOperatorVtable){.op=PG_CASCADES_PHYSICAL_HASHAGG,.name="HashAgg",.cost_fn=pg_cost_agg_fn,.build_plan_fn=pg_build_agg_fn,.derive_stats_fn=pg_derive_agg_stats_fn});pg_registry_register_operator(&(PgOperatorVtable){.op=PG_CASCADES_PHYSICAL_GROUPAGG,.name="GroupAgg",.cost_fn=pg_cost_agg_fn,.build_plan_fn=pg_build_agg_fn,.derive_stats_fn=pg_derive_agg_stats_fn});pg_registry_register_rule(PG_RULE_MEMO_IMPL,"LogicalAgg→PhysicalHashAgg",PG_CASCADES_LOGICAL_AGG,PG_CASCADES_PHYSICAL_HASHAGG,g_pat_leaf2,pg_rule_agg_to_hashagg,0.8,PG_RULE_BIT_AGG_TO_HASHAGG);pg_registry_register_rule(PG_RULE_MEMO_IMPL,"LogicalAgg→PhysicalGroupAgg",PG_CASCADES_LOGICAL_AGG,PG_CASCADES_PHYSICAL_GROUPAGG,NULL,pg_rule_agg_to_groupagg,0.7,PG_RULE_BIT_AGG_TO_GROUPAGG);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"MergeTwoAgg",PG_CASCADES_LOGICAL_AGG,0,g_pat_agg_agg_leaf,pg_rule_merge_two_agg,0.5,PG_RULE_BIT_MERGE_TWO_AGG);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"PruneAggColumns",PG_CASCADES_LOGICAL_AGG,0,NULL,pg_rule_prune_agg_columns,0.5,PG_RULE_BIT_PRUNE_AGG_COLS);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"PushDownAggLimit",PG_CASCADES_LOGICAL_AGG,0,g_pat_agg_limit_leaf,pg_rule_pushdown_agg_limit,0.4,PG_RULE_BIT_PUSHDOWN_AGG_LIMIT);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"EliminateAgg",PG_CASCADES_LOGICAL_AGG,0,NULL,pg_rule_eliminate_agg,0.6,PG_RULE_BIT_ELIMINATE_AGG);}
