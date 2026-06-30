#include "postgres.h"
#include "optimizer/planmain.h"
#include "optimizer/cascades.h"
#include "core/registry.h"
extern PgPattern *g_pat_leaf1,*g_pat_project_project_leaf;

static void pg_cost_project_fn(PgPlannerCascadesContext *ctx,PgMemoGroup *group,PgGroupExpr *expr,double input_rows,int input_width,Cost child_startup,Cost child_total,Cost *out_startup,Cost *out_total){*out_startup=child_startup;*out_total=child_total;}

static Plan *
pg_build_project_fn(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
                    PgGroupExpr *expr, PgGroupBestEntry *best,
                    PgRequiredProperty *required, PgOutputProperty *output)
{
    Plan*child;PgGroupBestEntry*child_best=NULL;PgOutputProperty child_out;
    PgMemoGroup*child_group;ListCell*lc;
    if(expr->inputs==NIL)return NULL;
    child_group=(PgMemoGroup*)linitial(expr->inputs);
    if(child_group->best_entries==NIL)return NULL;
    foreach(lc,child_group->best_entries){PgGroupBestEntry*e=(PgGroupBestEntry*)lfirst(lc);if(best->child_required_props!=NIL&&e->expr->mode==PG_PHYS_EXPR_COMPOSABLE_OP&&pg_required_property_equal(e->required,pg_safe_linitial_child_req(best))){child_best=e;break;}}
    if(child_best==NULL){foreach(lc,child_group->best_entries){PgGroupBestEntry*e=(PgGroupBestEntry*)lfirst(lc);if(best->child_required_props!=NIL&&pg_required_property_equal(e->required,pg_safe_linitial_child_req(best))){child_best=e;break;}}}
    if(child_best==NULL)return NULL;
    child=pg_cascades_build_plan_recurse(ctx,child_group,pg_safe_linitial_child_req(best),child_best,&child_out);
    if(child==NULL)return NULL;
    *output=best->output;
    /* Always wrap with Result to ensure correct column projection */
    return(Plan*)make_result(ctx->root,ctx->upper->sub_tlist,NULL,child);
}

List *pg_rule_project_to_physical_project(PgPlannerCascadesContext *ctx, PgGroupExpr *expr){PgGroupExpr*r=pg_memo_new_group_expr(ctx,PG_CASCADES_PHYSICAL_PROJECT);r->mode=PG_PHYS_EXPR_COMPOSABLE_OP;r->inputs=expr->inputs;r->op_private=ctx->upper;return list_make1(r);}
List *pg_rule_merge_project_with_child(PgPlannerCascadesContext *ctx, PgGroupExpr *expr){PgMemoGroup*cg;PgGroupExpr*in=NULL;PgGroupExpr*np;ListCell*lc;if(expr->inputs==NIL)return NIL;cg=(PgMemoGroup*)linitial(expr->inputs);if(cg==NULL||cg->logical_exprs==NIL)return NIL;foreach(lc,cg->logical_exprs){PgGroupExpr*e=(PgGroupExpr*)lfirst(lc);if(e->op==PG_CASCADES_LOGICAL_PROJECT){in=e;break;}}if(in==NULL)return NIL;np=pg_memo_new_group_expr(ctx,PG_CASCADES_LOGICAL_PROJECT);np->inputs=in->inputs;np->op_private=expr->op_private;return list_make1(np);}
List *pg_rule_eliminate_project(PgPlannerCascadesContext *ctx, PgGroupExpr *expr){PgMemoGroup*cg;PgGroupExpr*ch=NULL;ListCell*lc;if(expr->inputs==NIL)return NIL;cg=(PgMemoGroup*)linitial(expr->inputs);foreach(lc,cg->logical_exprs){PgGroupExpr*e=(PgGroupExpr*)lfirst(lc);if(e->op==PG_CASCADES_LOGICAL_PROJECT){ch=e;break;}}if(ch==NULL)return NIL;if(expr->op_private==ch->op_private){PgGroupExpr*ne=pg_memo_new_group_expr(ctx,PG_CASCADES_LOGICAL_PROJECT);ne->inputs=ch->inputs;ne->op_private=ch->op_private;return list_make1(ne);}return NIL;}
List *pg_rule_prune_empty_union(PgPlannerCascadesContext *ctx, PgGroupExpr *expr){PgMemoGroup*cg;PgGroupExpr*uo=NULL;ListCell*lc;if(expr->inputs==NIL)return NIL;cg=(PgMemoGroup*)linitial(expr->inputs);foreach(lc,cg->logical_exprs){PgGroupExpr*e=(PgGroupExpr*)lfirst(lc);if(e->op==PG_CASCADES_LOGICAL_UNION){uo=e;break;}}if(uo==NULL)return NIL;return list_make1(uo);}
List *pg_rule_prune_project_columns(PgPlannerCascadesContext *ctx, PgGroupExpr *expr){List*tlist=(List*)expr->op_private;ListCell*lc;Bitmapset*n=NULL;if(tlist==NIL)return NIL;foreach(lc,tlist){TargetEntry*te=(TargetEntry*)lfirst(lc);if(te->expr!=NULL)pg_collect_all_var_attnos((Node*)te->expr,&n);}if(n!=NULL){if(expr->owner_group->logical_prop.output_columns!=NULL)n=bms_union(n,expr->owner_group->logical_prop.output_columns);expr->owner_group->logical_prop.output_columns=n;}return NIL;}

void pg_module_project_init(void){pg_registry_register_operator(&(PgOperatorVtable){.op=PG_CASCADES_PHYSICAL_PROJECT,.name="Project",.cost_fn=pg_cost_project_fn,.build_plan_fn=pg_build_project_fn});pg_registry_register_rule(PG_RULE_MEMO_IMPL,"LogicalProject→PhysicalProject",PG_CASCADES_LOGICAL_PROJECT,PG_CASCADES_PHYSICAL_PROJECT,g_pat_leaf1,pg_rule_project_to_physical_project,1.0,PG_RULE_BIT_PROJECT_TO_PROJECT);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"MergeProjectWithChild",PG_CASCADES_LOGICAL_PROJECT,0,g_pat_project_project_leaf,pg_rule_merge_project_with_child,0.5,PG_RULE_BIT_MERGE_PROJECT);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"EliminateProject",PG_CASCADES_LOGICAL_PROJECT,0,NULL,pg_rule_eliminate_project,0.6,PG_RULE_BIT_ELIMINATE_PROJECT);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"PruneProjectColumns",PG_CASCADES_LOGICAL_PROJECT,0,NULL,pg_rule_prune_project_columns,0.6,PG_RULE_BIT_PRUNE_PROJ_COLS);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"PruneEmptyUnion",PG_CASCADES_LOGICAL_PROJECT,0,NULL,pg_rule_prune_empty_union,0.8,PG_RULE_BIT_PRUNE_EMPTY_UNION);}
