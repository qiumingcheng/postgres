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

bool
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

void
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
 * Custom Var walker for correlation var extraction
 *
 * PG 9.2's pull_var_clause has subtle behavioral differences; this
 * custom walker reliably extracts Vars from the subquery's WHERE
 * clause, separating inner vars (varlevelsup=0) from correlated
 * outer vars (varlevelsup>0).
 * ======================================================================== */

typedef struct PgVarCollectCtx
{
    List *inner_vars;    /* varlevelsup == 0 */
    List *outer_vars;    /* varlevelsup > 0 */
} PgVarCollectCtx;

static bool
pg_collect_correlation_vars_walker(Node *node, void *context)
{
    PgVarCollectCtx *ctx = (PgVarCollectCtx *) context;

    if (node == NULL)
        return false;

    if (IsA(node, Var))
    {
        Var *v = (Var *) node;
        if (v->varattno <= 0)
            return false;
        if (v->varlevelsup == 0)
            ctx->inner_vars = list_append_unique_ptr(ctx->inner_vars, v);
        else if (v->varlevelsup > 0)
            ctx->outer_vars = list_append_unique_ptr(ctx->outer_vars, v);
        return false;
    }

    return expression_tree_walker(node, pg_collect_correlation_vars_walker,
                                   context);
}

static void
pg_collect_correlation_vars(Node *node, List **inner, List **outer)
{
    PgVarCollectCtx ctx;
    ctx.inner_vars = NIL;
    ctx.outer_vars = NIL;
    pg_collect_correlation_vars_walker(node, &ctx);
    *inner = ctx.inner_vars;
    *outer = ctx.outer_vars;
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

    if (sublink->subselect == NULL || !IsA(sublink->subselect, Query))
        return false;

    subquery = (Query *) sublink->subselect;
    if (subquery->jointree == NULL || subquery->jointree->fromlist == NIL)
        return false;

    /* Extract correlation Vars using custom walker (more reliable than
     * pull_var_clause for PG 9.2). */
    if (subquery->jointree->quals != NULL)
        pg_collect_correlation_vars((Node *) subquery->jointree->quals,
                                     &corr_inner, &corr_outer);

    /* Also scan targetList for correlation vars */
    {
        ListCell *tlc;
        foreach(tlc, subquery->targetList)
        {
            TargetEntry *te2 = (TargetEntry *) lfirst(tlc);
            List *t_inner = NIL, *t_outer = NIL;
            pg_collect_correlation_vars((Node *) te2->expr, &t_inner, &t_outer);
            corr_outer = list_concat_unique_ptr(corr_outer, t_outer);
            list_free(t_inner);
            list_free(t_outer);
        }
    }

    if (list_length(corr_outer) != 1)
    {
        if (cascades_planner_debug)
            elog(NOTICE, "Cascades decorrelation: need exactly 1 outer var "
                 "(got %d)", list_length(corr_outer));
        return false;
    }
    if (list_length(corr_inner) < 1)
    {
        if (cascades_planner_debug)
            elog(NOTICE, "Cascades decorrelation: need at least 1 inner var");
        return false;
    }

    /*
     * TODO: Full decorrelation pipeline (GROUP BY + LEFT JOIN).
     * The custom var walker above correctly identifies correlation
     * variables. The remaining work is to:
     *   1. Add GROUP BY on inner_var to subquery (with proper ressortgroupref)
     *   2. Remove correlation from subquery WHERE
     *   3. Register subquery as new RTE via addRangeTableEntryForSubquery
     *   4. Wrap parent FROM in LEFT JOIN with properly-looked-up join qual
     *   5. Replace the SubLink target entry with a Var referencing the subquery
     *
     * Known issue: the LEFT JOIN approach produces "variable not found
     * in subplan target lists" from setrefs.c because manually-constructed
     * join quals don't survive PG's plan reference fixing.  This needs
     * deeper integration with PG's query tree construction.
     */
    if (cascades_planner_debug)
        elog(NOTICE, "Cascades decorrelation: found %d outer / %d inner "
             "correlation vars; full pipeline not yet implemented",
             list_length(corr_outer), list_length(corr_inner));
    return false;
}
