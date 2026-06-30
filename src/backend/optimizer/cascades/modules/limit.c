#include "postgres.h"
#include "optimizer/planmain.h"
#include "optimizer/cascades.h"
#include "core/registry.h"
extern PgPattern *g_pat_leaf1,*g_pat_limit_sort_leaf,*g_pat_limit_join_leaf_leaf;

static void pg_cost_limit_fn(PgPlannerCascadesContext *ctx,PgMemoGroup *group,PgGroupExpr *expr,double input_rows,int input_width,Cost child_startup,Cost child_total,Cost *out_startup,Cost *out_total){double frac=1.0;if(ctx->upper->limit_tuples>0&&ctx->upper->limit_tuples<input_rows)frac=ctx->upper->limit_tuples/input_rows;*out_startup=child_startup;*out_total=child_startup+(child_total-child_startup)*frac;}

static Plan *
pg_build_limit_fn(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
                  PgGroupExpr *expr, PgGroupBestEntry *best,
                  PgRequiredProperty *required, PgOutputProperty *output)
{
    Plan *child; PgGroupBestEntry *child_best=NULL; PgOutputProperty child_out;
    PgMemoGroup *child_group; ListCell *lc;
    if(expr->inputs==NIL)return NULL;
    child_group=(PgMemoGroup*)linitial(expr->inputs);
    if(child_group->best_entries==NIL)return NULL;
    /* Pass 1: prefer COMPOSABLE_OP */
    foreach(lc,child_group->best_entries){PgGroupBestEntry*e=(PgGroupBestEntry*)lfirst(lc);if(best->child_required_props!=NIL&&e->expr->mode==PG_PHYS_EXPR_COMPOSABLE_OP&&pg_required_property_equal(e->required,pg_safe_linitial_child_req(best))){child_best=e;break;}}
    if(child_best==NULL){foreach(lc,child_group->best_entries){PgGroupBestEntry*e=(PgGroupBestEntry*)lfirst(lc);if(best->child_required_props!=NIL&&pg_required_property_equal(e->required,pg_safe_linitial_child_req(best))){child_best=e;break;}}}
    if(child_best==NULL)child_best=(PgGroupBestEntry*)linitial(child_group->best_entries);
    child=pg_cascades_build_plan_recurse(ctx,child_group,pg_safe_linitial_child_req(best),child_best,&child_out);
    if(child==NULL)return NULL;
    *output=best->output;
    return(Plan*)make_limit(child,ctx->root->parse->limitOffset,ctx->root->parse->limitCount,ctx->upper->offset_est,ctx->upper->count_est);
}

List *pg_rule_limit_to_physical_limit(PgPlannerCascadesContext *ctx, PgGroupExpr *expr){PgGroupExpr *r=pg_memo_new_group_expr(ctx,PG_CASCADES_PHYSICAL_LIMIT);r->mode=PG_PHYS_EXPR_COMPOSABLE_OP;r->inputs=expr->inputs;r->op_private=ctx->upper;return list_make1(r);}
List *pg_rule_merge_limit_with_sort(PgPlannerCascadesContext *ctx, PgGroupExpr *expr){PgGroupExpr *se=pg_memo_group_first_logical((PgMemoGroup*)linitial(expr->inputs),PG_CASCADES_LOGICAL_SORT);if(se==NULL||ctx->upper->limit_tuples<=0)return NIL;PgSortPrivate *sp=(PgSortPrivate*)palloc(sizeof(PgSortPrivate));sp->pathkeys=(List*)se->op_private;sp->limit_tuples=ctx->upper->limit_tuples;PgGroupExpr *r=pg_memo_new_group_expr(ctx,PG_CASCADES_LOGICAL_SORT);r->inputs=se->inputs;r->op_private=sp;return list_make1(r);}
List *pg_rule_pushdown_limit_join(PgPlannerCascadesContext *ctx, PgGroupExpr *expr){PgGroupExpr *je=pg_memo_group_first_logical((PgMemoGroup*)linitial(expr->inputs),PG_CASCADES_LOGICAL_JOIN);if(je==NULL)return NIL;PgJoinPrivate *jp=(PgJoinPrivate*)je->op_private;if(jp==NULL||jp->jointype!=JOIN_INNER||ctx->upper->limit_tuples<=0)return NIL;PgGroupExpr *lo=pg_memo_new_group_expr(ctx,PG_CASCADES_LOGICAL_LIMIT);lo->inputs=list_make1(linitial(je->inputs));lo->op_private=ctx->upper;PgGroupExpr *li=pg_memo_new_group_expr(ctx,PG_CASCADES_LOGICAL_LIMIT);li->inputs=list_make1(lsecond(je->inputs));li->op_private=ctx->upper;PgGroupExpr *nj=pg_memo_new_group_expr(ctx,PG_CASCADES_LOGICAL_JOIN);nj->inputs=list_make2(linitial(je->inputs),lsecond(je->inputs));nj->op_private=jp;return list_make3(lo,li,nj);}
List *pg_rule_eliminate_limit(PgPlannerCascadesContext *ctx, PgGroupExpr *expr){Query *p=ctx->root->parse;if(p->limitCount!=NULL||p->limitOffset!=NULL)return NIL;PgMemoGroup *cg=(PgMemoGroup*)linitial(expr->inputs);if(cg==NULL||cg==expr->owner_group)return NIL;pg_memo_merge_group(ctx,expr->owner_group,cg);expr->owner_group->logical_exprs=list_delete_ptr(expr->owner_group->logical_exprs,expr);return NIL;}
List *pg_rule_merge_limit_with_child_limit(PgPlannerCascadesContext *ctx, PgGroupExpr *expr){PgMemoGroup *cg=(PgMemoGroup*)linitial(expr->inputs);PgGroupExpr *il=NULL;ListCell *lc;foreach(lc,cg->logical_exprs){PgGroupExpr *e=(PgGroupExpr*)lfirst(lc);if(e->op==PG_CASCADES_LOGICAL_LIMIT){il=e;break;}}if(il==NULL)return NIL;PgGroupExpr *r=pg_memo_new_group_expr(ctx,PG_CASCADES_LOGICAL_LIMIT);r->inputs=il->inputs;r->op_private=expr->op_private;return list_make1(r);}

void pg_module_limit_init(void){pg_registry_register_operator(&(PgOperatorVtable){.op=PG_CASCADES_PHYSICAL_LIMIT,.name="Limit",.cost_fn=pg_cost_limit_fn,.build_plan_fn=pg_build_limit_fn});pg_registry_register_rule(PG_RULE_MEMO_IMPL,"LogicalLimit→PhysicalLimit",PG_CASCADES_LOGICAL_LIMIT,PG_CASCADES_PHYSICAL_LIMIT,g_pat_leaf1,pg_rule_limit_to_physical_limit,1.0,PG_RULE_BIT_LIMIT_TO_LIMIT);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"MergeLimitWithSort",PG_CASCADES_LOGICAL_LIMIT,0,g_pat_limit_sort_leaf,pg_rule_merge_limit_with_sort,0.7,PG_RULE_BIT_MERGE_LIMIT_SORT);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"PushDownLimitJoin",PG_CASCADES_LOGICAL_LIMIT,0,g_pat_limit_join_leaf_leaf,pg_rule_pushdown_limit_join,0.4,PG_RULE_BIT_PUSHDOWN_LIMIT_JOIN);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"EliminateLimit",PG_CASCADES_LOGICAL_LIMIT,0,NULL,pg_rule_eliminate_limit,0.6,PG_RULE_BIT_ELIMINATE_LIMIT);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"MergeLimitWithChildLimit",PG_CASCADES_LOGICAL_LIMIT,0,NULL,pg_rule_merge_limit_with_child_limit,0.45,PG_RULE_BIT_MERGE_LIMIT_CHILD_LIMIT);}
