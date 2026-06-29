#include "postgres.h"
#include "optimizer/planmain.h"
#include "optimizer/cascades.h"
#include "core/registry.h"
extern PgPattern *g_pat_leaf1,*g_pat_project_project_leaf;
static void pg_cost_project_fn(PgPlannerCascadesContext *ctx,PgMemoGroup *group,PgGroupExpr *expr,double input_rows,int input_width,Cost child_startup,Cost child_total,Cost *out_startup,Cost *out_total){*out_startup=child_startup;*out_total=child_total;}
static Plan *pg_build_project_fn(PgPlannerCascadesContext *ctx,PgMemoGroup *group,PgGroupExpr *expr,PgGroupBestEntry *best,PgRequiredProperty *required,PgOutputProperty *output){Plan*child;PgGroupBestEntry*child_best=NULL;PgOutputProperty child_out;PgMemoGroup*child_group;ListCell*lc;if(expr->inputs==NIL)return NULL;child_group=(PgMemoGroup*)linitial(expr->inputs);if(child_group->best_entries==NIL)return NULL;foreach(lc,child_group->best_entries){PgGroupBestEntry*e=(PgGroupBestEntry*)lfirst(lc);if(best->child_required_props!=NIL&&pg_required_property_equal(e->required,pg_safe_linitial_child_req(best))){child_best=e;break;}}if(child_best==NULL)return NULL;child=pg_cascades_build_plan_recurse(ctx,child_group,pg_safe_linitial_child_req(best),child_best,&child_out);if(child==NULL)return NULL;*output=best->output;if(ctx->upper->numGroupCols==0&&!ctx->upper->hasAggs)return child;return(Plan*)make_result(ctx->root,ctx->upper->sub_tlist,NULL,child);}

/* === pg_rule_project_to_physical_project (moved from rule.c) === */

List *
pg_rule_project_to_physical_project(PgPlannerCascadesContext *ctx,
                                     PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_PROJECT);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = expr->inputs;
    result->op_private = ctx->upper;
    return list_make1(result);
}


/* === pg_rule_merge_project_with_child (moved from rule.c) === */

List *
pg_rule_merge_project_with_child(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *child_group;
    PgGroupExpr *inner = NULL;
    PgGroupExpr *new_proj;
    ListCell   *lc;

    if (expr->inputs == NIL)
        return NIL;

    child_group = (PgMemoGroup *) linitial(expr->inputs);
    if (child_group == NULL || child_group->logical_exprs == NIL)
        return NIL;

    /* Find LogicalProject in child group's logical expressions */
    foreach(lc, child_group->logical_exprs)
    {
        PgGroupExpr *e = (PgGroupExpr *) lfirst(lc);
        if (e->op == PG_CASCADES_LOGICAL_PROJECT)
        {
            inner = e;
            break;
        }
    }

    if (inner == NULL)
        return NIL;

    /* 新 Project 直接用外层的 tlist，child 指向内层的 child */
    new_proj = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_PROJECT);
    new_proj->inputs = inner->inputs;       /* 跳过内层 Project */
    new_proj->op_private = expr->op_private; /* 外层 tlist */
    return list_make1(new_proj);
}


/* === pg_rule_eliminate_project (moved from rule.c) === */

List *
pg_rule_eliminate_project(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *child_group;
    PgGroupExpr *child = NULL;
    ListCell   *lc;

    if (expr->inputs == NIL)
        return NIL;

    child_group = (PgMemoGroup *) linitial(expr->inputs);

    /* Find LogicalProject in child group's logical expressions */
    foreach(lc, child_group->logical_exprs)
    {
        PgGroupExpr *e = (PgGroupExpr *) lfirst(lc);
        if (e->op == PG_CASCADES_LOGICAL_PROJECT)
        {
            child = e;
            break;
        }
    }

    /* 如果 child 不是 Project，无法判断 tlist 是否相同 —— 保守保留 */
    if (child == NULL)
        return NIL;

    /*
     * 简单启发式：如果外层 tlist 和内层 tlist 指针相同（由 pg_adapter 保证），
     * 则消除外层 Project。
     * 更精确的检查需要逐项比较 TargetEntry，第一版用指针相等。
     */
    if (expr->op_private == child->op_private)
    {
        /* 创建新的 expression，跳过当前 Project */
        PgGroupExpr *new_expr = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_PROJECT);
        new_expr->inputs = child->inputs;
        new_expr->op_private = child->op_private;
        return list_make1(new_expr);
    }

    return NIL;
}


/* === pg_rule_prune_empty_union (moved from rule.c) === */

List *
pg_rule_prune_empty_union(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    ListCell   *lc;
    List       *non_empty = NIL;
    bool        any_empty = false;

    foreach(lc, expr->inputs)
    {
        PgMemoGroup *child = (PgMemoGroup *) lfirst(lc);

        if (child->rows == 0 && child->width == 0)
        {
            any_empty = true;
            continue;  /* 跳过空分支 */
        }
        non_empty = lappend(non_empty, child);
    }

    if (!any_empty)
        return NIL;

    if (list_length(non_empty) == 0)
    {
        /* 所有分支都空 → 标记当前 group 为空 */
        expr->owner_group->rows = 0;
        expr->owner_group->width = 0;
        return NIL;
    }

    if (list_length(non_empty) == 1)
    {
        /* 只剩一个分支 → 创建透传 expression */
        PgGroupExpr *new_expr = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_PROJECT);
        new_expr->inputs = non_empty;
        new_expr->op_private = ctx->upper->tlist;
        return list_make1(new_expr);
    }

    /* 多个非空分支 → 重建 Union */
    {
        PgGroupExpr *new_union = pg_memo_new_group_expr(ctx,
                                        PG_CASCADES_LOGICAL_PROJECT);
        new_union->inputs = non_empty;
        new_union->op_private = expr->op_private;
        return list_make1(new_union);
    }
}


/* === pg_rule_prune_project_columns (moved from rule.c) === */

List *
pg_rule_prune_project_columns(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgCascadesUpperInfo *upper = ctx->upper;
    Bitmapset  *needed = NULL;
    ListCell   *lc;

    if (upper == NULL || upper->tlist == NIL)
        return NIL;

    /* Start from parent-required columns (from logical_prop) */
    if (expr->owner_group->logical_prop.output_columns != NULL)
        needed = bms_copy(expr->owner_group->logical_prop.output_columns);

    /* Map each target entry: if parent needs resno X, find what child vars
     * contribute to that expression.  First version: walk tlist entries
     * and pull all Var references whose resno is in needed. */
    foreach(lc, upper->tlist)
    {
        TargetEntry *te = (TargetEntry *) lfirst(lc);
        if (needed != NULL && !bms_is_member(te->resno, needed))
            continue;
        /* Include all Var references used in this target entry */
        pg_collect_all_var_attnos((Node *) te->expr, &needed);
    }

    /* Propagate needed columns to child group */
    if (needed != NULL && list_length(expr->inputs) >= 1)
    {
        PgMemoGroup *child = (PgMemoGroup *) linitial(expr->inputs);
        if (child->logical_prop.output_columns != NULL)
            needed = bms_union(needed, child->logical_prop.output_columns);
        child->logical_prop.output_columns = needed;
    }
    else if (needed != NULL)
        bms_free(needed);

    return NIL;
}

void pg_module_project_init(void){pg_registry_register_operator(&(PgOperatorVtable){.op=PG_CASCADES_PHYSICAL_PROJECT,.name="Project",.cost_fn=pg_cost_project_fn,.build_plan_fn=pg_build_project_fn});pg_registry_register_rule(PG_RULE_MEMO_IMPL,"LogicalProject→PhysicalProject",PG_CASCADES_LOGICAL_PROJECT,PG_CASCADES_PHYSICAL_PROJECT,g_pat_leaf1,pg_rule_project_to_physical_project,1.0,PG_RULE_BIT_PROJECT_TO_PROJECT);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"MergeProjectWithChild",PG_CASCADES_LOGICAL_PROJECT,0,g_pat_project_project_leaf,pg_rule_merge_project_with_child,0.5,PG_RULE_BIT_MERGE_PROJECT);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"EliminateProject",PG_CASCADES_LOGICAL_PROJECT,0,NULL,pg_rule_eliminate_project,0.6,PG_RULE_BIT_ELIMINATE_PROJECT);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"PruneProjectColumns",PG_CASCADES_LOGICAL_PROJECT,0,NULL,pg_rule_prune_project_columns,0.6,PG_RULE_BIT_PRUNE_PROJ_COLS);pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"PruneEmptyUnion",PG_CASCADES_LOGICAL_PROJECT,0,NULL,pg_rule_prune_empty_union,0.8,PG_RULE_BIT_PRUNE_EMPTY_UNION);}
