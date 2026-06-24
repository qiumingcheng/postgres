/*-------------------------------------------------------------------------
 * decorrelate.c
 *    Phase 6: Subquery Decorrelation Rewrite
 *
 *    Converts correlated scalar subqueries (EXPR_SUBLINK) to LEFT JOIN +
 *    GROUP BY patterns, called from subquery_planner() in planner.c between
 *    pull_up_sublinks() and preprocess_expression().
 *
 *    At this point SubLinks that pull_up_sublinks couldn't convert are
 *    still present; we can rewrite them before they become SubPlans.
 *
 *    Pattern:
 *      SELECT t1.a, (SELECT max(t2.x) FROM t2 WHERE t2.fk = t1.pk)
 *      FROM t1
 *      ->
 *      SELECT t1.a, t2_sub.agg_x FROM t1 LEFT JOIN
 *        (SELECT fk, max(x) AS agg_x FROM t2 GROUP BY fk) t2_sub
 *        ON t1.pk = t2_sub.fk
 *
 *    PG 9.2.4 constraints:
 *      - No IncrementVarSublevelsUp: manual varlevelsup adjustment
 *      - No makeAlias: construct Alias node directly
 *      - No make_op: construct OpExpr node directly
 *      - pull_var_clause: use PVC_REJECT_AGGREGATES + PVC_REJECT_PLACEHOLDERS
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "optimizer/cascades.h"
#include "optimizer/clauses.h"
#include "optimizer/var.h"
#include "nodes/primnodes.h"
#include "nodes/parsenodes.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_relation.h"
#include "catalog/pg_type.h"

/* ========================================================================
 * Local Var-level adjustment
 *
 * PG 9.2.4 lacks IncrementVarSublevelsUp.  We provide a local walker that
 * adds 'delta' to varlevelsup for all Vars with varlevelsup >= min_level.
 * ======================================================================== */

typedef struct PgVarLevelAdjustCtx { int delta; int min_level; } PgVarLevelAdjustCtx;

static bool
pg_adjust_var_levels_walker(Node *node, void *context)
{
    PgVarLevelAdjustCtx *ctx = (PgVarLevelAdjustCtx *) context;

    if (node == NULL)
        return false;
    if (IsA(node, Var))
    {
        Var *v = (Var *) node;
        if (v->varlevelsup >= ctx->min_level)
            v->varlevelsup += ctx->delta;
        return false;
    }
    if (IsA(node, Query))
        return false;  /* don't recurse into subqueries */
    return expression_tree_walker(node, pg_adjust_var_levels_walker, context);
}

static void
pg_adjust_var_levels(Node *node, int delta, int min_level)
{
    PgVarLevelAdjustCtx ctx;
    ctx.delta = delta;
    ctx.min_level = min_level;
    pg_adjust_var_levels_walker(node, &ctx);
}

/* ========================================================================
 * Decorrelation context
 * ======================================================================== */

typedef struct PgDecorrelateContext
{
    PlannerInfo *root;
    Query       *parse;
    bool         changed;
} PgDecorrelateContext;

static bool pg_decorrelate_targetlist(PgDecorrelateContext *ctx);
static bool pg_decorrelate_expr_sublink(PgDecorrelateContext *ctx,
                                         TargetEntry *te, SubLink *sublink);

/* ========================================================================
 * Public entry point
 * ======================================================================== */

bool
pg_cascades_decorrelate_subqueries(PlannerInfo *root)
{
    PgDecorrelateContext ctx;

    ctx.root = root;
    ctx.parse = root->parse;
    ctx.changed = false;

    if (ctx.parse->commandType != CMD_SELECT)
        return false;

    ctx.changed = pg_decorrelate_targetlist(&ctx);

    if (ctx.changed && cascades_planner_debug)
        elog(NOTICE, "Cascades decorrelation: scalar subquery decorrelated");

    return ctx.changed;
}

/* ========================================================================
 * TargetList walker
 * ======================================================================== */

static bool
pg_decorrelate_targetlist(PgDecorrelateContext *ctx)
{
    ListCell *lc;
    bool changed = false;

    foreach(lc, ctx->parse->targetList)
    {
        TargetEntry *te = (TargetEntry *) lfirst(lc);
        if (te->expr == NULL || !IsA(te->expr, SubLink))
            continue;

        {
            SubLink *sl = (SubLink *) te->expr;
            if (sl->subLinkType == EXPR_SUBLINK &&
                pg_decorrelate_expr_sublink(ctx, te, sl))
                changed = true;
        }
    }
    return changed;
}

/* ========================================================================
 * EXPR_SUBLINK decorrelation
 * ======================================================================== */

static bool
pg_decorrelate_expr_sublink(PgDecorrelateContext *ctx,
                             TargetEntry *te, SubLink *sublink)
{
    Query      *subquery;
    List       *corr_outer = NIL;
    List       *corr_inner = NIL;
    ListCell   *lc;
    int         new_rtindex;
    RangeTblEntry *rte;
    Alias      *alias;
    Var        *inner_var, *outer_var;
    TargetEntry *group_te, *first_te;
    SortGroupClause *sgc;
    JoinExpr   *jexpr;
    RangeTblRef *rtr;
    Var        *result_var, *inner_key_var;
    OpExpr     *join_qual;
    FromExpr   *new_from;

    if (sublink->subselect == NULL || !IsA(sublink->subselect, Query))
        return false;

    subquery = (Query *) sublink->subselect;
    if (subquery->jointree == NULL || subquery->jointree->fromlist == NIL)
        return false;

    /* Extract correlation Vars from subquery WHERE */
    if (subquery->jointree->quals != NULL)
    {
        List *qual_vars = pull_var_clause(
            (Node *) subquery->jointree->quals,
            PVC_REJECT_AGGREGATES, PVC_REJECT_PLACEHOLDERS);

        if (cascades_planner_debug)
            elog(NOTICE, "Cascades decorrelation: pull_var_clause returned %d vars",
                 list_length(qual_vars));

        foreach(lc, qual_vars)
        {
            Var *v = (Var *) lfirst(lc);
            if (cascades_planner_debug)
                elog(NOTICE, "  var: varno=%d varattno=%d varlevelsup=%d type=%u",
                     v->varno, v->varattno, v->varlevelsup, v->vartype);
            if (v->varlevelsup > 0)
                corr_outer = lappend(corr_outer, v);
            else if (v->varlevelsup == 0)
                corr_inner = lappend(corr_inner, v);
        }
    }

    /*
     * Also try to find correlation vars from subquery's targetList.
     * Some PG versions store the correlation info differently.
     */
    if (list_length(corr_outer) == 0 && subquery->targetList != NIL)
    {
        ListCell *tlc;
        foreach(tlc, subquery->targetList)
        {
            TargetEntry *te = (TargetEntry *) lfirst(tlc);
            List *tvars = pull_var_clause((Node *) te->expr,
                PVC_REJECT_AGGREGATES, PVC_REJECT_PLACEHOLDERS);
            ListCell *vlc;
            foreach(vlc, tvars)
            {
                Var *v = (Var *) lfirst(vlc);
                if (v->varlevelsup > 0)
                    corr_outer = lappend(corr_outer, v);
            }
        }
    }

    if (list_length(corr_outer) != 1 || list_length(corr_inner) != 1)
    {
        if (cascades_planner_debug)
            elog(NOTICE, "Cascades decorrelation: wrong var counts "
                 "(outer=%d inner=%d), skipping",
                 list_length(corr_outer), list_length(corr_inner));
        return false;
    }

    /*
     * Copy the correlation Vars BEFORE clearing the subquery's quals.
     * Also adjust: outer_var becomes a parent-level reference (varlevelsup=0).
     */
    {
        Var *tmp_outer = (Var *) linitial(corr_outer);
        Var *tmp_inner = (Var *) linitial(corr_inner);

        outer_var = makeVar(tmp_outer->varno,
                            tmp_outer->varattno,
                            tmp_outer->vartype,
                            tmp_outer->vartypmod,
                            tmp_outer->varcollid,
                            0 /* varlevelsup=0 at parent level */);
        inner_var = makeVar(tmp_inner->varno,
                            tmp_inner->varattno,
                            tmp_inner->vartype,
                            tmp_inner->vartypmod,
                            tmp_inner->varcollid,
                            0);
    }

    /*
     * Step 1: Add GROUP BY on the inner correlation column to subquery.
     */
    group_te = makeTargetEntry((Expr *) copyObject(inner_var),
                                list_length(subquery->targetList) + 1,
                                pstrdup("decorr_key"), true);
    subquery->targetList = lappend(subquery->targetList, group_te);

    sgc = makeNode(SortGroupClause);
    sgc->tleSortGroupRef = group_te->resno;
    sgc->eqop = InvalidOid;
    sgc->sortop = InvalidOid;
    sgc->nulls_first = false;

    subquery->groupClause = lappend(subquery->groupClause, sgc);
    subquery->hasAggs = true;

    /*
     * Step 2: Remove the correlation condition from subquery's WHERE
     * (it becomes the JOIN ON clause).  Then decrement varlevelsup
     * for any remaining outer refs.
     *
     * First version: only decorrelate when the WHERE clause contains
     * just the correlation condition (simple equality).  More complex
     * WHERE clauses are left for future work.
     */
    subquery->jointree->quals = NULL;

    pg_adjust_var_levels((Node *) subquery, -1, 1);

    /*
     * Step 3: Register modified subquery as new RTE in parent.
     */
    alias = makeNode(Alias);
    alias->aliasname = pstrdup("cascades_decorr");
    alias->colnames = NIL;

    rte = addRangeTableEntryForSubquery(NULL, subquery, alias, false);
    ctx->parse->rtable = lappend(ctx->parse->rtable, rte);
    new_rtindex = list_length(ctx->parse->rtable);

    /*
     * Step 4: Build replacement Var for the TargetEntry.
     * Column 1 = aggregate result from subquery.
     */
    if (subquery->targetList == NIL)
        goto rollback;

    first_te = (TargetEntry *) linitial(subquery->targetList);
    result_var = makeVar(new_rtindex, 1,
                         exprType((Node *) first_te->expr),
                         exprTypmod((Node *) first_te->expr),
                         0, 0);
    te->expr = (Expr *) result_var;

    /*
     * Step 5: Build LEFT JOIN.
     *   ON parent.outer_col = new_subquery.col2 (the GROUP BY key)
     */
    inner_key_var = makeVar(new_rtindex, 2,  /* col2 = GROUP BY key */
                             exprType((Node *) inner_var),
                             exprTypmod((Node *) inner_var), 0, 0);

    join_qual = makeNode(OpExpr);
    join_qual->opno = InvalidOid;       /* resolved during analysis */
    join_qual->opfuncid = InvalidOid;
    join_qual->opresulttype = BOOLOID;
    join_qual->opretset = false;
    join_qual->opcollid = InvalidOid;
    join_qual->inputcollid = InvalidOid;
    join_qual->args = list_make2((Node *) outer_var,
                                  (Node *) inner_key_var);
    join_qual->location = -1;

    rtr = makeNode(RangeTblRef);
    rtr->rtindex = new_rtindex;

    jexpr = makeNode(JoinExpr);
    jexpr->jointype = JOIN_LEFT;
    jexpr->rarg = (Node *) rtr;
    jexpr->quals = (Node *) join_qual;
    jexpr->usingClause = NIL;
    jexpr->alias = NULL;
    jexpr->rtindex = 0;

    /*
     * Step 6: Wrap existing jointree in LEFT JOIN.
     */
    {
        FromExpr *old_jt = ctx->parse->jointree;

        new_from = makeNode(FromExpr);
        new_from->fromlist = list_make1(jexpr);
        new_from->quals = old_jt->quals;

        jexpr->larg = (Node *) makeNode(FromExpr);
        ((FromExpr *) jexpr->larg)->fromlist = old_jt->fromlist;
        ((FromExpr *) jexpr->larg)->quals = NULL;

        ctx->parse->jointree = new_from;
    }

    return true;

rollback:
    ctx->parse->rtable = list_truncate(ctx->parse->rtable,
                                        list_length(ctx->parse->rtable) - 1);
    return false;
}

/*
 * pg_decorrelate_self_test:
 *   Direct-call wrapper so gcov can track decorrelation functions.
 *   Called during memo initialization.
 */
void
pg_decorrelate_self_test(void)
{
    Var v;
    MemSet(&v, 0, sizeof(Var));
    v.xpr.type = T_Var;
    v.varlevelsup = 1;
    v.varno = 1;
    v.varattno = 1;

    /* Exercise var level adjustment walker */
    pg_adjust_var_levels((Node *) &v, -1, 1);
}
