#include "postgres.h"
#include "optimizer/cascades.h"
#include "core/registry.h"
#include "optimizer/cost.h"
extern PgPattern *g_pat_join_leaf_leaf, *g_pat_join_join_leaf_leaf_leaf, *g_pat_join_leaf_join_leaf_leaf;
/* forward */
RelOptInfo *pg_rule_join_get_child_rel(PgPlannerCascadesContext *ctx, PgMemoGroup *child_grp);
extern PgPattern *g_pat_filter_join_leaf_leaf, *g_pat_join_filter_leaf_leaf, *g_pat_join_project_leaf_project_leaf;

/* === pg_rule_join_to_nestloop (moved from rule.c) === */

List *
pg_rule_join_to_nestloop(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *outer_grp, *inner_grp;

    outer_grp = (PgMemoGroup *) linitial(expr->inputs);
    inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

    /* Safety: skip if either child is empty (pruned by rewrite) */
    if (outer_grp == NULL || inner_grp == NULL)
        return NIL;
    if ((outer_grp->rows == 0 && outer_grp->width == 0) ||
        (inner_grp->rows == 0 && inner_grp->width == 0))
        return NIL;

    /*
     * Always create COMPOSABLE_OP NESTLOOP — the cost function
     * (cost_nestloop) adds disable_cost when enable_nestloop=off,
     * making it a last resort that's only chosen when no other join
     * method is viable (e.g., cross joins).  This matches PG's
     * behavior: NestPaths are always generated, never disabled.
     */
    {
        PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                      PG_CASCADES_PHYSICAL_NESTLOOP);
        result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
        result->inputs = expr->inputs;
        result->op_private = expr->op_private;
        return list_make1(result);
    }
}


/* === pg_rule_join_to_hashjoin (moved from rule.c) === */

List *
pg_rule_join_to_hashjoin(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *outer_grp, *inner_grp;

    outer_grp = (PgMemoGroup *) linitial(expr->inputs);
    inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

    if (outer_grp == NULL || inner_grp == NULL)
        return NIL;
    if ((outer_grp->rows == 0 && outer_grp->width == 0) ||
        (inner_grp->rows == 0 && inner_grp->width == 0))
        return NIL;

    if (!enable_hashjoin)
        return NIL;

    {
        PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                      PG_CASCADES_PHYSICAL_HASHJOIN);
        result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
        result->inputs = expr->inputs;
        result->op_private = expr->op_private;
        return list_make1(result);
    }
}


/* === pg_rule_join_to_mergejoin (moved from rule.c) === */

List *
pg_rule_join_to_mergejoin(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *outer_grp, *inner_grp;

    outer_grp = (PgMemoGroup *) linitial(expr->inputs);
    inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

    if (outer_grp == NULL || inner_grp == NULL)
        return NIL;
    if ((outer_grp->rows == 0 && outer_grp->width == 0) ||
        (inner_grp->rows == 0 && inner_grp->width == 0))
        return NIL;

    if (!enable_mergejoin)
        return NIL;

    {
        PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                      PG_CASCADES_PHYSICAL_MERGEJOIN);
        result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
        result->inputs = expr->inputs;
        result->op_private = expr->op_private;
        return list_make1(result);
    }
}


/* === pg_rule_join_commutativity (moved from rule.c) === */

List *
pg_rule_join_commutativity(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *new_expr;
    PgMemoGroup *left_child;
    PgMemoGroup *right_child;

    left_child = (PgMemoGroup *) linitial(expr->inputs);
    right_child = (PgMemoGroup *) lsecond(expr->inputs);

    if (left_child == NULL || right_child == NULL)
        return NIL;

    /* Don't swap if both children are the same (self-join handled elsewhere) */
    if (left_child == right_child)
        return NIL;

    new_expr = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    new_expr->inputs = list_make2(right_child, left_child);  /* swapped! */
    new_expr->op_private = expr->op_private;

    return list_make1(new_expr);
}


/* === pg_rule_merge_filter_with_join (moved from rule.c) === */

List *
pg_rule_merge_filter_with_join(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *outer_input;
    PgGroupExpr *new_join;
    PgJoinPrivate *join_priv;

    join_priv = (PgJoinPrivate *) expr->op_private;
    if (join_priv == NULL || join_priv->jointype != JOIN_INNER)
        return NIL;

    outer_input = pg_memo_group_first_logical(
        (PgMemoGroup *) linitial(expr->inputs), PG_CASCADES_LOGICAL_FILTER);

    /* 只处理 outer 侧有 Filter 的情况 */
    if (outer_input == NULL)
        return NIL;

    {
        List *extra_quals = (List *) outer_input->op_private;
        ListCell *lc;

        /* 检查是否有 volatile function — 有则不合并 */
        foreach(lc, extra_quals)
        {
            RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
            if (contain_volatile_functions((Node *) ri->clause))
                return NIL;
        }

        /* 将 filter quals 合并到 join restrictlist */
        join_priv->restrictlist = list_concat(join_priv->restrictlist,
                                               extra_quals);
    }

    /* 构建新 LogicalJoin：child 改为 Filter 的 child（跳过 Filter） */
    new_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    new_join->inputs = list_make2(linitial(outer_input->inputs),  /* Filter 的 child */
                                   lsecond(expr->inputs));        /* inner 不变 */
    new_join->op_private = join_priv;

    return list_make1(new_join);
}


/* === pg_rule_merge_join_with_child_project (moved from rule.c) === */

List *
pg_rule_merge_join_with_child_project(PgPlannerCascadesContext *ctx,
                                       PgGroupExpr *expr)
{
    PgGroupExpr *outer_input, *inner_input;
    bool outer_is_proj, inner_is_proj;
    PgGroupExpr *new_join;

    outer_input = pg_memo_group_first_logical(
        (PgMemoGroup *) linitial(expr->inputs), PG_CASCADES_LOGICAL_PROJECT);
    inner_input = pg_memo_group_first_logical(
        (PgMemoGroup *) lsecond(expr->inputs), PG_CASCADES_LOGICAL_PROJECT);

    outer_is_proj = (outer_input != NULL);
    inner_is_proj = (inner_input != NULL);

    if (!outer_is_proj && !inner_is_proj)
        return NIL;

    new_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    new_join->inputs = list_make2(
        outer_is_proj ? linitial(outer_input->inputs) : (void *) outer_input,
        inner_is_proj ? linitial(inner_input->inputs) : (void *) inner_input);
    new_join->op_private = expr->op_private;

    return list_make1(new_join);
}


/* === pg_rule_prune_empty_join (moved from rule.c) === */

List *
pg_rule_prune_empty_join(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *outer_grp, *inner_grp;

    outer_grp = (PgMemoGroup *) linitial(expr->inputs);
    inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

    if ((outer_grp->rows == 0 && outer_grp->width == 0) ||
        (inner_grp->rows == 0 && inner_grp->width == 0))
    {
        expr->owner_group->rows = 0;
        expr->owner_group->width = 0;
    }

    return NIL;
}


/* === pg_rule_prune_join_columns (moved from rule.c) === */

List *
pg_rule_prune_join_columns(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgJoinPrivate *join_priv = (PgJoinPrivate *) expr->op_private;
    Bitmapset  *needed = NULL;
    ListCell   *lc;

    if (join_priv == NULL)
        return NIL;

    /* Collect columns from join quals (join key columns) */
    foreach(lc, join_priv->restrictlist)
    {
        RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
        pg_collect_all_var_attnos((Node *) ri->clause, &needed);
    }

    /*
     * Also propagate parent-required output_columns (from B4/B3/B5)
     * down to children so B1 can see them.  Join quals (restrictlist)
     * may be empty since PG handles quals internally via RelOptInfo.
     */
    if (expr->owner_group->logical_prop.output_columns != NULL)
    {
        needed = bms_union(needed,
                    expr->owner_group->logical_prop.output_columns);
    }

    if (needed != NULL)
    {
        expr->owner_group->logical_prop.output_columns = needed;

        /* Propagate needed columns down to each child so B1 can see them */
        foreach(lc, expr->inputs)
        {
            PgMemoGroup *child = (PgMemoGroup *) lfirst(lc);
            if (child->logical_prop.output_columns != NULL)
                child->logical_prop.output_columns =
                    bms_union(child->logical_prop.output_columns, needed);
            else
                child->logical_prop.output_columns = bms_copy(needed);
        }
    }
    return NIL;
}


/* === pg_rule_eliminate_join_with_constant (moved from rule.c) === */

List *
pg_rule_eliminate_join_with_constant(PgPlannerCascadesContext *ctx,
                                      PgGroupExpr *expr)
{
    PgJoinPrivate *join_priv;
    PgMemoGroup *outer_grp, *inner_grp;

    join_priv = (PgJoinPrivate *) expr->op_private;
    if (join_priv == NULL || join_priv->jointype != JOIN_INNER)
        return NIL;

    outer_grp = (PgMemoGroup *) linitial(expr->inputs);
    inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

    /* Check if either side is a single-row relation */
    if (outer_grp->rel != NULL && outer_grp->rel->rows <= 1.0 &&
        outer_grp->rel->tuples <= 1)
    {
        /* Outer is constant — return inner directly via Project */
        PgGroupExpr *new_proj = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_PROJECT);
        new_proj->inputs = list_make1(inner_grp);
        new_proj->op_private = ctx->upper->tlist;
        return list_make1(new_proj);
    }

    if (inner_grp->rel != NULL && inner_grp->rel->rows <= 1.0 &&
        inner_grp->rel->tuples <= 1)
    {
        PgGroupExpr *new_proj = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_PROJECT);
        new_proj->inputs = list_make1(outer_grp);
        new_proj->op_private = ctx->upper->tlist;
        return list_make1(new_proj);
    }

    return NIL;
}


/* === pg_rule_outer_join_elimination (moved from rule.c) === */

List *
pg_rule_outer_join_elimination(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgJoinPrivate *join_priv;
    PgJoinPrivate *new_priv;
    PgGroupExpr *new_join;

    join_priv = (PgJoinPrivate *) expr->op_private;
    if (join_priv == NULL)
        return NIL;

    /* Only transform LEFT JOIN */
    if (join_priv->jointype != JOIN_LEFT)
        return NIL;

    /* First version: always convert to INNER (PG's reduce_outer_joins
     * already handles the truly-eliminable cases) */
    new_priv = (PgJoinPrivate *) palloc(sizeof(PgJoinPrivate));
    memcpy(new_priv, join_priv, sizeof(PgJoinPrivate));
    new_priv->jointype = JOIN_INNER;

    new_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    new_join->inputs = expr->inputs;
    new_join->op_private = new_priv;

    return list_make1(new_join);
}


/* === pg_rule_join_associativity (moved from rule.c) === */

List *
pg_rule_join_associativity(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *left_grp, *right_grp;
    PgGroupExpr *inner_join;
    PgMemoGroup *A_grp, *B_grp, *C_grp;
    PgGroupExpr *bc_join, *abc_join;
    PgMemoGroup *bc_group;
    ListCell   *lc;

    /* expr is (A⋈B)⋈C: JOIN with 2 inputs */
    left_grp = (PgMemoGroup *) linitial(expr->inputs);
    right_grp = (PgMemoGroup *) lsecond(expr->inputs);

    if (left_grp == NULL || right_grp == NULL)
        return NIL;

    /* Find a JOIN expression inside left_grp.
     * After rewrite pipeline + Final Cleanup group merge, LOGICAL_JOIN
     * may have been eliminated from logical_exprs.  Search physical_exprs
     * too — COMPOSABLE_OP join expressions have valid child inputs. */
    inner_join = NULL;
    foreach(lc, left_grp->logical_exprs)
    {
        PgGroupExpr *e = (PgGroupExpr *) lfirst(lc);
        if (e->op == PG_CASCADES_LOGICAL_JOIN &&
            list_length(e->inputs) == 2)
        {
            inner_join = e;
            break;
        }
    }

    if (inner_join == NULL)
    {
        /* Fallback: if group merge eliminated LOGICAL_JOIN from logical_exprs,
         * construct inner join from left_grp's PG joinrel.  Iterate base
         * groups in memo to find A and B by matching relids. */
        Relids left_relids = pg_cascades_group_relids(ctx, left_grp);
        if (left_relids != NULL)
        {
            PgMemoGroup *found[2];
            int nfound = 0;
            ListCell *gc;

            MemSet(found, 0, sizeof(found));
            foreach(gc, ctx->memo->groups)
            {
                PgMemoGroup *g = (PgMemoGroup *) lfirst(gc);
                if (g != left_grp && g != right_grp &&
                    g->rel != NULL && g->rel->relids != NULL &&
                    bms_overlap(g->rel->relids, left_relids))
                {
                    if (nfound < 2)
                        found[nfound++] = g;
                }
            }

            if (nfound >= 2 && found[0] != NULL && found[1] != NULL)
            {
                inner_join = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_JOIN);
                inner_join->inputs = list_make2(found[0], found[1]);
            }
        }
        if (left_relids != NULL)
            bms_free(left_relids);
    }

    if (inner_join == NULL)
        return NIL;

    A_grp = (PgMemoGroup *) linitial(inner_join->inputs);
    B_grp = (PgMemoGroup *) lsecond(inner_join->inputs);
    C_grp = right_grp;

    if (A_grp == NULL || B_grp == NULL || C_grp == NULL)
        return NIL;

    /* Avoid degenerate: don't reassociate if any two inputs are the same */
    if (A_grp == B_grp || A_grp == C_grp || B_grp == C_grp)
        return NIL;

    /*
     * Step 1: Create (B⋈C), insert into Memo to get its group.
     * StarRocks copyIn pattern: transform returns a tree of OptExpression,
     * copyIn recursively creates groups.  Here we do it explicitly.
     */
    bc_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    bc_join->inputs = list_make2(B_grp, C_grp);
    /* op_private stays NULL — quals resolved from PG joinrel at impl time */

    bc_group = pg_memo_insert_expression(ctx, ctx->memo, bc_join, NULL);
    if (bc_group == NULL)
        return NIL;  /* (B⋈C) already exists or duplicate */

    /*
     * Step 2: Create A⋈(B⋈C) with bc_group as input.
     * The engine will push ExploreGroupTask for bc_group when this
     * expression is optimized.
     */
    abc_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    abc_join->inputs = list_make2(A_grp, bc_group);

    return list_make1(abc_join);
}


/* === pg_rule_join_left_asscom (moved from rule.c) === */

List *
pg_rule_join_left_asscom(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *left_grp, *right_grp;
    PgGroupExpr *inner_join;
    PgMemoGroup *A_grp, *B_grp, *C_grp;
    PgGroupExpr *ab_join, *abc_join;
    PgMemoGroup *ab_group;
    ListCell   *lc;

    /* expr is A⋈(B⋈C): JOIN with 2 inputs */
    left_grp = (PgMemoGroup *) linitial(expr->inputs);
    right_grp = (PgMemoGroup *) lsecond(expr->inputs);

    if (left_grp == NULL || right_grp == NULL)
        return NIL;

    /* Find a JOIN expression inside right_grp */
    inner_join = NULL;
    foreach(lc, right_grp->logical_exprs)
    {
        PgGroupExpr *e = (PgGroupExpr *) lfirst(lc);
        if (e->op == PG_CASCADES_LOGICAL_JOIN &&
            list_length(e->inputs) == 2)
        {
            inner_join = e;
            break;
        }
    }
    /* Relids-based fallback: same as JoinAssociativity (C2).
     * After Final Cleanup group merge, LOGICAL_JOIN may be gone from
     * logical_exprs.  Find base groups by matching relids. */
    if (inner_join == NULL)
    {
        Relids right_relids = pg_cascades_group_relids(ctx, right_grp);
        if (right_relids != NULL)
        {
            PgMemoGroup *found[2];
            int nfound = 0;
            ListCell *gc;

            MemSet(found, 0, sizeof(found));
            foreach(gc, ctx->memo->groups)
            {
                PgMemoGroup *g = (PgMemoGroup *) lfirst(gc);
                if (g != left_grp && g != right_grp &&
                    g->rel != NULL && g->rel->relids != NULL &&
                    bms_overlap(g->rel->relids, right_relids))
                {
                    if (nfound < 2)
                        found[nfound++] = g;
                }
            }

            if (nfound >= 2 && found[0] != NULL && found[1] != NULL)
            {
                inner_join = pg_memo_new_group_expr(ctx,
                                    PG_CASCADES_LOGICAL_JOIN);
                inner_join->inputs = list_make2(found[0], found[1]);
            }
        }
        if (right_relids != NULL)
            bms_free(right_relids);
    }

    if (inner_join == NULL)
        return NIL;

    A_grp = left_grp;
    B_grp = (PgMemoGroup *) linitial(inner_join->inputs);
    C_grp = (PgMemoGroup *) lsecond(inner_join->inputs);

    if (A_grp == NULL || B_grp == NULL || C_grp == NULL)
        return NIL;

    /* Avoid degenerate */
    if (A_grp == B_grp || A_grp == C_grp || B_grp == C_grp)
        return NIL;

    /*
     * Step 1: Create (A⋈B), insert to get its group.
     */
    ab_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    ab_join->inputs = list_make2(A_grp, B_grp);

    ab_group = pg_memo_insert_expression(ctx, ctx->memo, ab_join, NULL);
    if (ab_group == NULL)
        return NIL;

    /*
     * Step 2: Create (A⋈B)⋈C with ab_group as left input.
     */
    abc_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    abc_join->inputs = list_make2(ab_group, C_grp);

    return list_make1(abc_join);
}


/* === pg_rule_inner_to_semi (moved from rule.c) === */

List *
pg_rule_inner_to_semi(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *inner_grp;
    PgGroupExpr *new_join;
    PgJoinPrivate *join_priv_orig;
    PgJoinPrivate *join_priv_new;
    RelOptInfo  *inner_rel;

    inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

    /*
     * Check join type.  If op_private is NULL (legacy logical joins from
     * pg_adapter), we can't determine join type — skip.
     */
    join_priv_orig = (PgJoinPrivate *) expr->op_private;
    if (join_priv_orig == NULL)
        return NIL;

    if (join_priv_orig->jointype != JOIN_INNER)
        return NIL;

    /*
     * Check if inner side has a unique key.
     * For base relations: check group->rel for unique indexes.
     * For join relations: skip (first version).
     */
    inner_rel = inner_grp->rel;
    if (inner_rel == NULL)
        return NIL;

    /*
     * PostgreSQL stores unique indexes in the relation's indexlist.
     * We check if any index on this relation is unique/primary.
     * This is conservative: we don't verify that the join keys
     * actually match the unique columns.  A stricter check would
     * require comparing the join quals against the index columns.
     */
    if (inner_rel->indexlist != NIL)
    {
        ListCell *lc;
        bool has_unique = false;

        foreach(lc, inner_rel->indexlist)
        {
            IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
            if (idx->unique || idx->ncolumns == 1)
            {
                has_unique = true;
                break;
            }
        }

        if (!has_unique)
            return NIL;
    }
    else
    {
        /* No indexes at all — can't prove uniqueness */
        return NIL;
    }

    /*
     * Convert to SEMI: copy the join expr with jointype changed.
     * op_private is shallow-copied; we create a new PgJoinPrivate
     * to avoid mutating the original.
     */
    new_join = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_JOIN);
    new_join->inputs = list_copy(expr->inputs);
    join_priv_new = (PgJoinPrivate *) palloc(sizeof(PgJoinPrivate));
    memcpy(join_priv_new, join_priv_orig, sizeof(PgJoinPrivate));
    join_priv_new->jointype = JOIN_SEMI;
    new_join->op_private = join_priv_new;

    return list_make1(new_join);
}


/* === pg_rule_join_to_hashjoin_phase4 (moved from rule.c) === */

List *
pg_rule_join_to_hashjoin_phase4(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgJoinPrivate *join_priv;
    PgMemoGroup *outer_grp, *inner_grp;
    RelOptInfo *outer_rel, *inner_rel, *joinrel;
    List *result = NIL;
    ListCell *lc;

    if (expr == NULL || expr->inputs == NIL)
        return NIL;

    join_priv = (PgJoinPrivate *) expr->op_private;
    if (join_priv == NULL || join_priv->jointype != JOIN_INNER)
        return NIL;

    outer_grp = (PgMemoGroup *) linitial(expr->inputs);
    inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

    outer_rel = pg_rule_join_get_child_rel(ctx, outer_grp);
    inner_rel = pg_rule_join_get_child_rel(ctx, inner_grp);

    if (outer_rel == NULL || inner_rel == NULL)
        return NIL;

    joinrel = make_join_rel(ctx->root, outer_rel, inner_rel);
    if (joinrel == NULL)
        return NIL;

    set_cheapest(joinrel);

    foreach(lc, joinrel->pathlist)
    {
        Path *path = (Path *) lfirst(lc);
        if (path->pathtype == T_HashJoin)
        {
            PgGroupExpr *phys = pg_memo_new_group_expr(ctx,
                                        PG_CASCADES_PHYSICAL_HASHJOIN);
            phys->mode = PG_PHYS_EXPR_IMPORTED_PATH;
            phys->op_private = path;
            phys->inputs = NIL;
            result = lappend(result, phys);
        }
    }
    return result;
}


/* === pg_rule_join_to_nestloop_phase4 (moved from rule.c) === */

List *
pg_rule_join_to_nestloop_phase4(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgJoinPrivate *join_priv;
    PgMemoGroup *outer_grp, *inner_grp;
    RelOptInfo *outer_rel, *inner_rel, *joinrel;
    List *result = NIL;
    ListCell *lc;

    if (expr == NULL || expr->inputs == NIL)
        return NIL;

    join_priv = (PgJoinPrivate *) expr->op_private;
    if (join_priv == NULL || join_priv->jointype != JOIN_INNER)
        return NIL;

    outer_grp = (PgMemoGroup *) linitial(expr->inputs);
    inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

    outer_rel = pg_rule_join_get_child_rel(ctx, outer_grp);
    inner_rel = pg_rule_join_get_child_rel(ctx, inner_grp);

    if (outer_rel == NULL || inner_rel == NULL)
        return NIL;

    joinrel = make_join_rel(ctx->root, outer_rel, inner_rel);
    if (joinrel == NULL)
        return NIL;

    set_cheapest(joinrel);

    foreach(lc, joinrel->pathlist)
    {
        Path *path = (Path *) lfirst(lc);
        if (path->pathtype == T_NestLoop)
        {
            PgGroupExpr *phys = pg_memo_new_group_expr(ctx,
                                        PG_CASCADES_PHYSICAL_NESTLOOP);
            phys->mode = PG_PHYS_EXPR_IMPORTED_PATH;
            phys->op_private = path;
            phys->inputs = NIL;
            result = lappend(result, phys);
        }
    }
    return result;
}


/* === pg_rule_join_to_mergejoin_phase4 (moved from rule.c) === */

List *
pg_rule_join_to_mergejoin_phase4(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgJoinPrivate *join_priv;
    PgMemoGroup *outer_grp, *inner_grp;
    RelOptInfo *outer_rel, *inner_rel, *joinrel;
    List *result = NIL;
    ListCell *lc;

    if (expr == NULL || expr->inputs == NIL)
        return NIL;

    join_priv = (PgJoinPrivate *) expr->op_private;
    if (join_priv == NULL || join_priv->jointype != JOIN_INNER)
        return NIL;

    outer_grp = (PgMemoGroup *) linitial(expr->inputs);
    inner_grp = (PgMemoGroup *) lsecond(expr->inputs);

    outer_rel = pg_rule_join_get_child_rel(ctx, outer_grp);
    inner_rel = pg_rule_join_get_child_rel(ctx, inner_grp);

    if (outer_rel == NULL || inner_rel == NULL)
        return NIL;

    joinrel = make_join_rel(ctx->root, outer_rel, inner_rel);
    if (joinrel == NULL)
        return NIL;

    set_cheapest(joinrel);

    foreach(lc, joinrel->pathlist)
    {
        Path *path = (Path *) lfirst(lc);
        if (path->pathtype == T_MergeJoin)
        {
            PgGroupExpr *phys = pg_memo_new_group_expr(ctx,
                                        PG_CASCADES_PHYSICAL_MERGEJOIN);
            phys->mode = PG_PHYS_EXPR_IMPORTED_PATH;
            phys->op_private = path;
            phys->inputs = NIL;
            result = lappend(result, phys);
        }
    }
    return result;
}

/* Helper for Phase 4 join rules: find RelOptInfo for a memo group */
RelOptInfo *
pg_rule_join_get_child_rel(PgPlannerCascadesContext *ctx, PgMemoGroup *child_grp)
{
    if (child_grp->rel != NULL)
        return child_grp->rel;
    return pg_cascades_group_to_rel(ctx, child_grp);
}

void pg_module_join_init(void) {
    pg_registry_register_operator(&(PgOperatorVtable){.op=PG_CASCADES_PHYSICAL_NESTLOOP,.name="NestLoop"});
    pg_registry_register_operator(&(PgOperatorVtable){.op=PG_CASCADES_PHYSICAL_HASHJOIN,.name="HashJoin"});
    pg_registry_register_operator(&(PgOperatorVtable){.op=PG_CASCADES_PHYSICAL_MERGEJOIN,.name="MergeJoin"});
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,"LogicalJoin→PhysicalNestLoop",PG_CASCADES_LOGICAL_JOIN,PG_CASCADES_PHYSICAL_NESTLOOP,g_pat_join_leaf_leaf,pg_rule_join_to_nestloop,0.5,PG_RULE_BIT_JOIN_TO_NESTLOOP);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,"LogicalJoin→PhysicalHashJoin",PG_CASCADES_LOGICAL_JOIN,PG_CASCADES_PHYSICAL_HASHJOIN,g_pat_join_leaf_leaf,pg_rule_join_to_hashjoin,0.8,PG_RULE_BIT_JOIN_TO_HASHJOIN);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,"LogicalJoin→PhysicalMergeJoin",PG_CASCADES_LOGICAL_JOIN,PG_CASCADES_PHYSICAL_MERGEJOIN,g_pat_join_leaf_leaf,pg_rule_join_to_mergejoin,0.6,PG_RULE_BIT_JOIN_TO_MERGEJOIN);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,"LogicalJoin→PhysicalHashJoin_Phase4",PG_CASCADES_LOGICAL_JOIN,PG_CASCADES_PHYSICAL_HASHJOIN,NULL,pg_rule_join_to_hashjoin_phase4,0.9,PG_RULE_BIT_JOIN_TO_HASHJOIN_PHASE4);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,"LogicalJoin→PhysicalNestLoop_Phase4",PG_CASCADES_LOGICAL_JOIN,PG_CASCADES_PHYSICAL_NESTLOOP,NULL,pg_rule_join_to_nestloop_phase4,0.9,PG_RULE_BIT_JOIN_TO_NESTLOOP_PHASE4);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,"LogicalJoin→PhysicalMergeJoin_Phase4",PG_CASCADES_LOGICAL_JOIN,PG_CASCADES_PHYSICAL_MERGEJOIN,NULL,pg_rule_join_to_mergejoin_phase4,0.9,PG_RULE_BIT_JOIN_TO_MERGEJOIN_PHASE4);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"JoinCommutativity",PG_CASCADES_LOGICAL_JOIN,0,g_pat_join_leaf_leaf,pg_rule_join_commutativity,0.5,PG_RULE_BIT_JOIN_COMMUTATIVITY);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"JoinAssociativity",PG_CASCADES_LOGICAL_JOIN,0,g_pat_join_join_leaf_leaf_leaf,pg_rule_join_associativity,0.2,PG_RULE_BIT_JOIN_ASSOCIATIVITY);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"JoinLeftAsscom",PG_CASCADES_LOGICAL_JOIN,0,g_pat_join_leaf_join_leaf_leaf,pg_rule_join_left_asscom,0.2,PG_RULE_BIT_JOIN_LEFT_ASSCOM);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"MergeFilterWithJoin",PG_CASCADES_LOGICAL_JOIN,0,g_pat_join_filter_leaf_leaf,pg_rule_merge_filter_with_join,0.4,PG_RULE_BIT_MERGE_FILTER_JOIN);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"PruneEmptyJoin",PG_CASCADES_LOGICAL_JOIN,0,NULL,pg_rule_prune_empty_join,0.9,PG_RULE_BIT_PRUNE_EMPTY_JOIN);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"PruneJoinColumns",PG_CASCADES_LOGICAL_JOIN,0,NULL,pg_rule_prune_join_columns,0.5,PG_RULE_BIT_PRUNE_JOIN_COLS);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"EliminateJoinWithConst",PG_CASCADES_LOGICAL_JOIN,0,NULL,pg_rule_eliminate_join_with_constant,0.7,PG_RULE_BIT_ELIM_JOIN_CONST);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"OuterJoinElimination",PG_CASCADES_LOGICAL_JOIN,0,NULL,pg_rule_outer_join_elimination,0.6,PG_RULE_BIT_OUTER_JOIN_ELIM);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"InnerToSemi",PG_CASCADES_LOGICAL_JOIN,0,NULL,pg_rule_inner_to_semi,0.3,PG_RULE_BIT_INNER_TO_SEMI);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"MergeJoinWithChildProj",PG_CASCADES_LOGICAL_JOIN,0,g_pat_join_project_leaf_project_leaf,pg_rule_merge_join_with_child_project,0.4,PG_RULE_BIT_MERGE_JOIN_PROJ);
}
