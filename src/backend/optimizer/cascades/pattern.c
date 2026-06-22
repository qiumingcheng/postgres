/*-------------------------------------------------------------------------
 * pattern.c
 *    Cascades Pattern Matching Engine (Phase 4)
 *
 *    Supports multi-node pattern matching:
 *      LEAF       — matches any GroupExpression
 *      MULTI_LEAF — matches any number of GroupExpressions (for UNION etc.)
 *      OPERATOR   — matches a specific operator kind
 *      TREE       — recursive subtree matching
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "nodes/pg_list.h"

/* ========================================================================
 * Pattern constructors
 * ======================================================================== */

PgPattern *
pg_pattern_leaf(void)
{
    PgPattern *p = (PgPattern *) palloc0(sizeof(PgPattern));
    p->type = PG_PATTERN_LEAF;
    p->children = NIL;
    return p;
}

PgPattern *
pg_pattern_multi_leaf(void)
{
    PgPattern *p = (PgPattern *) palloc0(sizeof(PgPattern));
    p->type = PG_PATTERN_MULTI_LEAF;
    p->children = NIL;
    return p;
}

PgPattern *
pg_pattern_op(PgCascadesOpKind op)
{
    PgPattern *p = (PgPattern *) palloc0(sizeof(PgPattern));
    p->type = PG_PATTERN_OPERATOR;
    p->op = op;
    p->children = NIL;
    return p;
}

PgPattern *
pg_pattern_tree(PgCascadesOpKind op, List *children)
{
    PgPattern *p = (PgPattern *) palloc0(sizeof(PgPattern));
    p->type = PG_PATTERN_TREE;
    p->op = op;
    p->children = children;
    return p;
}

/* ========================================================================
 * Core matching: pattern node → PgGroupExpr
 * ======================================================================== */

static PgBinder *
pg_pattern_match_to_expr(PgPattern *pattern, PgGroupExpr *expr)
{
    PgBinder *binder;

    switch (pattern->type)
    {
        case PG_PATTERN_LEAF:
        case PG_PATTERN_MULTI_LEAF:
            binder = (PgBinder *) palloc0(sizeof(PgBinder));
            binder->pattern = pattern;
            binder->expr = expr;
            binder->group = expr->owner_group;
            binder->child_matches = NIL;
            return binder;

        case PG_PATTERN_OPERATOR:
            if (expr->op != pattern->op)
                return NULL;
            binder = (PgBinder *) palloc0(sizeof(PgBinder));
            binder->pattern = pattern;
            binder->expr = expr;
            binder->group = expr->owner_group;
            binder->child_matches = NIL;
            return binder;

        case PG_PATTERN_TREE:
            {
                ListCell *pc, *ic;

                if (expr->op != pattern->op)
                    return NULL;
                if (list_length(pattern->children) != list_length(expr->inputs))
                    return NULL;

                binder = (PgBinder *) palloc0(sizeof(PgBinder));
                binder->pattern = pattern;
                binder->expr = expr;
                binder->group = expr->owner_group;
                binder->child_matches = NIL;

                forboth(pc, pattern->children, ic, expr->inputs)
                {
                    PgPattern  *chpat = (PgPattern *) lfirst(pc);
                    void       *input = lfirst(ic);
                    PgBinder   *chb = NULL;
                    bool        ok = false;

                    if (input == NULL)
                    {
                        pfree(binder);
                        return NULL;
                    }

                    /* Try as PgGroupExpr first (rule-produced tree) */
                    chb = pg_pattern_match_to_expr(chpat,
                                                    (PgGroupExpr *) input);
                    if (chb != NULL)
                    {
                        binder->child_matches = lappend(
                            binder->child_matches, chb);
                        ok = true;
                    }
                    else
                    {
                        /* Try as PgMemoGroup (planbuild tree) */
                        PgMemoGroup *cg = (PgMemoGroup *) input;
                        ListCell    *cl;

                        foreach(cl, cg->logical_exprs)
                        {
                            PgGroupExpr *cex = (PgGroupExpr *) lfirst(cl);
                            chb = pg_pattern_match_to_expr(chpat, cex);
                            if (chb != NULL)
                            {
                                binder->child_matches = lappend(
                                    binder->child_matches, chb);
                                ok = true;
                                break;
                            }
                        }
                    }
                    if (!ok)
                    {
                        pfree(binder);
                        return NULL;
                    }
                }
                return binder;
            }

        default:
            return NULL;
    }
}

/* ========================================================================
 * Public matching API
 * ======================================================================== */

/*
 * pg_pattern_match_root_only:
 *   Match pattern against a single root expression only.
 *   Returns list of Binders (typically 0 or 1).
 */
List *
pg_pattern_match_root_only(PgPattern *pattern, PgGroupExpr *root)
{
    PgBinder *b = pg_pattern_match_to_expr(pattern, root);
    if (b != NULL)
        return list_make1(b);
    return NIL;
}

/*
 * pg_pattern_match_full:
 *   Recursively expand all child-group expression combinations.
 *   For Phase 4 initial: delegates to root-only matching.
 *   Full expansion reserved for multi-node transformation rules.
 */
List *
pg_pattern_match_full(PgPattern *pattern, PgGroupExpr *root)
{
    return pg_pattern_match_root_only(pattern, root);
}
