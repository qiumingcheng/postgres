/*-------------------------------------------------------------------------
 * pattern.c
 *    Cascades Pattern Matching Engine (Phase 4)
 *
 *    Supports multi-node pattern matching:
 *      LEAF       — matches any GroupExpression
 *      MULTI_LEAF — matches any number of GroupExpressions
 *      TREE       — recursive subtree matching
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "nodes/pg_list.h"
#include "utils/memutils.h"

/* ========================================================================
 * Global pattern objects (allocated in TopMemoryContext, shared across rules)
 * ======================================================================== */
PgPattern *g_pat_leaf1 = NULL;
PgPattern *g_pat_leaf2 = NULL;
PgPattern *g_pat_join_leaf_leaf = NULL;
PgPattern *g_pat_join_join_leaf_leaf_leaf = NULL;
PgPattern *g_pat_join_leaf_join_leaf_leaf = NULL;
PgPattern *g_pat_filter_join_leaf_leaf = NULL;
PgPattern *g_pat_filter_project_leaf = NULL;
PgPattern *g_pat_limit_sort_leaf = NULL;
PgPattern *g_pat_limit_join_leaf_leaf = NULL;
PgPattern *g_pat_agg_agg_leaf = NULL;
PgPattern *g_pat_agg_limit_leaf = NULL;
PgPattern *g_pat_join_project_leaf_project_leaf = NULL;
PgPattern *g_pat_project_project_leaf = NULL;
PgPattern *g_pat_join_filter_leaf_leaf = NULL;


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

PgBinder *
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
 *
 *   Unlike pg_pattern_match_root_only which matches only against a single
 *   root expression, this function enumerates all expression combinations
 *   from child groups.
 *
 *   Example: For pattern Join(Leaf, Leaf) and root expr Join(A_grp, B_grp):
 *     - A_grp has logical_exprs: [Scan(t1), Filter(Scan(t1))]
 *     - B_grp has logical_exprs: [Scan(t2)]
 *     - Returns 2 Binders:
 *       Binder1: Join(Scan(t1), Scan(t2))
 *       Binder2: Join(Filter(Scan(t1)), Scan(t2))
 *
 *   The Binder tree mirrors the Pattern tree:
 *     root Binder.child_matches = [child_binder_list_1, child_binder_list_2, ...]
 *     where each child_binder_list_k is a List<PgBinder *> for the k-th child pattern.
 */
List *
pg_pattern_match_full(PgPattern *pattern, PgGroupExpr *root)
{
    List *results = NIL;

    if (pattern->type == PG_PATTERN_LEAF || pattern->type == PG_PATTERN_MULTI_LEAF)
    {
        /* Leaf: match any expression — return one binder */
        PgBinder *binder = (PgBinder *) palloc0(sizeof(PgBinder));
        binder->pattern = pattern;
        binder->expr = root;
        binder->group = root->owner_group;
        binder->child_matches = NIL;
        return list_make1(binder);
    }

    if (pattern->type == PG_PATTERN_TREE)
    {
        ListCell *pc, *ic;
        List *child_binder_lists;  /* List<List<List<PgBinder*>>> */
        List *child_combinations;  /* Cartesian product */
        ListCell *clc;

        /* Root op must match */
        if (root->op != pattern->op)
            return NIL;

        if (list_length(pattern->children) != list_length(root->inputs))
            return NIL;

        /*
         * Step 1: For each child pattern, collect all matching binders
         * from the corresponding child group.
         */
        child_binder_lists = NIL;  /* List of List<PgBinder *> */

        forboth(pc, pattern->children, ic, root->inputs)
        {
            PgPattern  *chpat = (PgPattern *) lfirst(pc);
            void       *input = lfirst(ic);
            List       *ch_binders = NIL;  /* List<PgBinder *> */

            if (input == NULL)
            {
                /* No child — only leaf patterns work here */
                if (chpat->type == PG_PATTERN_LEAF ||
                    chpat->type == PG_PATTERN_MULTI_LEAF)
                {
                    PgBinder *b = (PgBinder *) palloc0(sizeof(PgBinder));
                    b->pattern = chpat;
                    b->expr = NULL;
                    b->group = NULL;
                    b->child_matches = NIL;
                    ch_binders = list_make1(b);
                }
                else
                {
                    /* Non-leaf pattern cannot match NULL input */
                    list_free_deep(child_binder_lists);
                    return NIL;
                }
            }
            else
            {
                /*
                 * The input can be either:
                 *   - PgGroupExpr (rule-produced expression tree)
                 *   - PgMemoGroup (planbuild group)
                 *
                 * For PgMemoGroup: enumerate all logical expressions.
                 * For PgGroupExpr: match directly.
                 */

                /* Try as PgMemoGroup first (more common in full matching) */
                {
                    PgMemoGroup *cg = (PgMemoGroup *) input;
                    ListCell    *el;

                    /*
                     * Check if this looks like a PgMemoGroup: if it has
                     * logical_exprs, it's a group.  We use a heuristic:
                     * PgMemoGroup has an 'id' field; PgGroupExpr has an 'op' field.
                     * Since both start with different layouts, we can't safely
                     * cast without knowing.  We rely on the convention that
                     * PgGroupExpr->inputs contains PgMemoGroup* pointers.
                     *
                     * Safe approach: try matching each expression in the group.
                     */
                    foreach(el, cg->logical_exprs)
                    {
                        PgGroupExpr *cex = (PgGroupExpr *) lfirst(el);
                        PgBinder *b = pg_pattern_match_to_expr(chpat, cex);

                        if (b != NULL)
                            ch_binders = lappend(ch_binders, b);
                    }
                }

                /* If no matches via group, try direct expression match */
                if (ch_binders == NIL)
                {
                    PgBinder *b = pg_pattern_match_to_expr(chpat,
                                                            (PgGroupExpr *) input);
                    if (b != NULL)
                        ch_binders = list_make1(b);
                }
            }

            if (ch_binders == NIL)
            {
                /* Child pattern failed to match — no results for this root */
                list_free_deep(child_binder_lists);
                return NIL;
            }

            child_binder_lists = lappend(child_binder_lists, ch_binders);
        }

        /*
         * Step 2: Compute Cartesian product of all child binder lists.
         *
         * For n children with sizes s1, s2, ..., sn:
         *   Total combinations = s1 * s2 * ... * sn
         *
         * We use recursive enumeration to build all combinations.
         */
        child_combinations = NIL;  /* List<List<PgBinder *>> */

        {
            /*
             * Start with a single empty combination, then for each child,
             * replace each existing combination with |child_binders| copies
             * each extended by one child binder.
             */
            child_combinations = list_make1(NIL);  /* List<List<PgBinder*>> */

            foreach(clc, child_binder_lists)
            {
                List *ch_binders = (List *) lfirst(clc);
                List *new_combinations = NIL;
                ListCell *cc;

                foreach(cc, child_combinations)
                {
                    List *existing = (List *) lfirst(cc);
                    ListCell *cb;

                    foreach(cb, ch_binders)
                    {
                        PgBinder *b = (PgBinder *) lfirst(cb);
                        List *extended = list_copy(existing);
                        extended = lappend(extended, b);
                        new_combinations = lappend(new_combinations, extended);
                    }
                }

                /*
                 * Free old combinations list structure only.
                 * The PgBinder* data inside is still referenced by
                 * new_combinations (shallow-copied via list_copy).
                 */
                list_free(child_combinations);
                child_combinations = new_combinations;
            }
        }

        /*
         * Step 3: Create a Binder for each combination.
         */
        foreach(clc, child_combinations)
        {
            List *child_binders = (List *) lfirst(clc);
            PgBinder *binder = (PgBinder *) palloc0(sizeof(PgBinder));

            binder->pattern = pattern;
            binder->expr = root;
            binder->group = root->owner_group;
            binder->child_matches = child_binders;

            results = lappend(results, binder);
        }

        /*
         * Free the combination list (but not the binders, which are now
         * owned by results).
         */
        list_free(child_combinations);

        return results;
    }

    /* Unknown pattern type */
    return NIL;
}

/* === pg_cascades_init_rule_patterns (moved from rule.c) === */

void
pg_cascades_init_rule_patterns(void)
{
    int i;
    int num_rules;
    MemoryContext old_cxt;

    /* Already initialized */
    if (g_pat_leaf1 != NULL)
        return;

    /*
     * Allocate pattern objects in TopMemoryContext so they survive
     * the lifecycle of the Cascades memo context.  Without this,
     * g_pat_leaf1 becomes a dangling pointer when the memo context
     * is deleted, and the idempotency guard above sees != NULL,
     * causing use-after-free on subsequent planner calls.
     */
    old_cxt = MemoryContextSwitchTo(TopMemoryContext);

    /* Create shared leaf patterns */
    g_pat_leaf1 = pg_pattern_leaf();
    g_pat_leaf2 = pg_pattern_leaf();

    /* C2: JoinAssociativity — Join(Join(Leaf, Leaf), Leaf) */
    g_pat_join_join_leaf_leaf_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_JOIN,
        list_make2(
            pg_pattern_tree(PG_CASCADES_LOGICAL_JOIN,
                list_make2(pg_pattern_leaf(), pg_pattern_leaf())),
            pg_pattern_leaf()));

    /* C3: JoinLeftAsscom — Join(Leaf, Join(Leaf, Leaf)) */
    g_pat_join_leaf_join_leaf_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_JOIN,
        list_make2(
            pg_pattern_leaf(),
            pg_pattern_tree(PG_CASCADES_LOGICAL_JOIN,
                list_make2(pg_pattern_leaf(), pg_pattern_leaf()))));

    /* A2: PushDownPredicateJoin — Filter(Join(Leaf, Leaf)) */
    g_pat_filter_join_leaf_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_FILTER,
        list_make1(
            pg_pattern_tree(PG_CASCADES_LOGICAL_JOIN,
                list_make2(pg_pattern_leaf(), pg_pattern_leaf()))));

    /* A4: PushDownPredicateProject — Filter(Project(Leaf)) */
    g_pat_filter_project_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_FILTER,
        list_make1(
            pg_pattern_tree(PG_CASCADES_LOGICAL_PROJECT,
                list_make1(pg_pattern_leaf()))));

    /* D1: MergeLimitWithSort — Limit(Sort(Leaf)) */
    g_pat_limit_sort_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_LIMIT,
        list_make1(
            pg_pattern_tree(PG_CASCADES_LOGICAL_SORT,
                list_make1(pg_pattern_leaf()))));

    /* D2: PushDownLimitJoin — Limit(Join(Leaf, Leaf)) */
    g_pat_limit_join_leaf_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_LIMIT,
        list_make1(
            pg_pattern_tree(PG_CASCADES_LOGICAL_JOIN,
                list_make2(pg_pattern_leaf(), pg_pattern_leaf()))));

    /* F2: MergeTwoAgg — Agg(Agg(Leaf)) */
    g_pat_agg_agg_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_AGG,
        list_make1(
            pg_pattern_tree(PG_CASCADES_LOGICAL_AGG,
                list_make1(pg_pattern_leaf()))));

    /* F3: PushDownAggLimit — Agg(Limit(Leaf)) */
    g_pat_agg_limit_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_AGG,
        list_make1(
            pg_pattern_tree(PG_CASCADES_LOGICAL_LIMIT,
                list_make1(pg_pattern_leaf()))));

    /* G4: MergeJoinWithChildProject — Join(Project(Leaf), Project(Leaf)) */
    g_pat_join_project_leaf_project_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_JOIN,
        list_make2(
            pg_pattern_tree(PG_CASCADES_LOGICAL_PROJECT,
                list_make1(pg_pattern_leaf())),
            pg_pattern_tree(PG_CASCADES_LOGICAL_PROJECT,
                list_make1(pg_pattern_leaf()))));

    /* H2: MergeProjectWithChild — Project(Project(Leaf)) */
    g_pat_project_project_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_PROJECT,
        list_make1(
            pg_pattern_tree(PG_CASCADES_LOGICAL_PROJECT,
                list_make1(pg_pattern_leaf()))));

    /* H4: MergeFilterWithJoin — Join(Filter(Leaf), Leaf) */
    g_pat_join_filter_leaf_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_JOIN,
        list_make2(
            pg_pattern_tree(PG_CASCADES_LOGICAL_FILTER,
                list_make1(pg_pattern_leaf())),
            pg_pattern_leaf()));

    /* Also create simple Join(Leaf, Leaf) for JoinCommutativity */
    g_pat_join_leaf_leaf = pg_pattern_tree(PG_CASCADES_LOGICAL_JOIN,
        list_make2(pg_pattern_leaf(), pg_pattern_leaf()));

    /*
     * Patterns are assigned to rules at registration time via
     * pg_registry_register_rule() in each module's init function.
     * No static array patching needed.
     */

    MemoryContextSwitchTo(old_cxt);
}


/* === pg_cascades_init_combination_rules (moved from rule.c) === */

/* Combination rules — consumed by rewrite.c */
static PgCombinationRule g_combination_rules[] = {
    {"GP_PUSH_DOWN_PREDICATE", NULL, true},
    {"GP_PRUNE_COLUMNS", NULL, true},
    {"GP_JOIN_REORDER", NULL, true},
    {"GP_PRUNE_EMPTY", NULL, true},
    {NULL, NULL, false}  /* sentinel */
};

PgCombinationRule *
pg_cascades_get_combination_rules(int *num_rules)
{
    *num_rules = (int)(sizeof(g_combination_rules) / sizeof(PgCombinationRule)) - 1;
    return g_combination_rules;
}

void
pg_cascades_init_combination_rules(void)
{
    /* No dynamic initialization needed — combination rules are
     * driven by pg_cascades_logical_rewrite() at runtime. */
}

