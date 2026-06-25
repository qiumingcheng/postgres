/*-------------------------------------------------------------------------
 * property.c
 *    Cascades Property 操作：required property 比较、复制、满足判断、
 *    child property 派生、group best 更新
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/var.h"
#include "utils/memutils.h"

/* ========================================================================
 * RequiredProperty 操作
 * ======================================================================== */

bool
pg_required_property_equal(const PgRequiredProperty *a,
                           const PgRequiredProperty *b)
{
    if (a == NULL && b == NULL)
        return true;
    if (a == NULL || b == NULL)
        return false;

    /* pathkeys: 第一版用指针相等，后续可用 equal() */
    if (a->pathkeys != b->pathkeys)
    {
        /* 如果都不是 NIL，检查包含关系 */
        if (a->pathkeys != NIL && b->pathkeys != NIL)
        {
            if (!pathkeys_contained_in(a->pathkeys, b->pathkeys) ||
                !pathkeys_contained_in(b->pathkeys, a->pathkeys))
                return false;
        }
        else
            return false;
    }

    /* required_outer */
    if (!bms_equal(a->required_outer, b->required_outer))
        return false;

    /* tuple_fraction / limit_tuples: treat 0.0 as "not set" */
    if (a->tuple_fraction != b->tuple_fraction)
    {
        /* 0.0 means "not set by ENFORCE_AND_COST" — match any value */
        if (a->tuple_fraction > 0.0 && b->tuple_fraction > 0.0)
            return false;
    }
    if (a->limit_tuples != b->limit_tuples)
    {
        /* 0.0 means "not set" — match any value */
        if (a->limit_tuples > 0.0 && b->limit_tuples > 0.0)
            return false;
    }

    return true;
}

PgRequiredProperty *
pg_required_property_copy(PgPlannerCascadesContext *ctx,
                          const PgRequiredProperty *p)
{
    PgRequiredProperty *copy;

    copy = (PgRequiredProperty *) palloc0(sizeof(PgRequiredProperty));

    if (p != NULL)
    {
        copy->pathkeys = p->pathkeys;
        copy->required_outer = p->required_outer ?
            bms_copy(p->required_outer) : NULL;
        copy->tuple_fraction = p->tuple_fraction;
        copy->limit_tuples = p->limit_tuples;
    }

    return copy;
}

/* ========================================================================
 * 满足关系判断
 * ======================================================================== */

bool
pg_output_satisfies_required(const PgOutputProperty *out,
                              const PgRequiredProperty *req)
{
    /* pathkeys 满足: req 为空则任意满足；否则 out 必须包含 req */
    if (req->pathkeys != NIL)
    {
        if (out->pathkeys == NIL)
            return false;
        if (!pathkeys_contained_in(req->pathkeys, out->pathkeys))
            return false;
    }

    /* required_outer 满足: out 的依赖必须是 req 的子集 */
    if (req->required_outer != NULL)
    {
        if (out->required_outer == NULL)
            return false;
        if (!bms_is_subset(out->required_outer, req->required_outer))
            return false;
    }

    return true;
}

/* ========================================================================
 * Root Required Property
 * ======================================================================== */

/*
 * Helper: collect Var varattnos from a target list into a Bitmapset.
 */
static void
pg_collect_tlist_varattnos(List *tlist, Bitmapset **set)
{
    ListCell *lc;
    foreach(lc, tlist)
    {
        TargetEntry *te = (TargetEntry *) lfirst(lc);
        pull_varattnos((Node *) te->expr, 1, set);
    }
}

PgRequiredProperty *
pg_cascades_root_required_property(PgPlannerCascadesContext *ctx)
{
    PgCascadesUpperInfo *upper = ctx->upper;
    PgRequiredProperty *req;

    req = (PgRequiredProperty *) palloc0(sizeof(PgRequiredProperty));

    if (upper->sort_pathkeys != NIL)
        req->pathkeys = upper->sort_pathkeys;
    else if (upper->group_pathkeys != NIL)
        req->pathkeys = upper->group_pathkeys;
    else
        req->pathkeys = NIL;

    req->required_outer = NULL;
    req->tuple_fraction = upper->tuple_fraction;
    req->limit_tuples = upper->limit_tuples;

    /* Phase 5: populate required_columns from sub_tlist (the plan target list) */
    if (upper->sub_tlist != NIL)
        pg_collect_tlist_varattnos(upper->sub_tlist, &req->required_columns);

    return req;
}

/* ========================================================================
 * Child Property Derivation
 * ======================================================================== */

void
pg_derive_child_properties(PgPlannerCascadesContext *ctx,
                           PgGroupExpr *expr,
                           PgRequiredProperty *required,
                           List **child_required_props,
                           PgOutputProperty *output)
{
    PgRequiredProperty *child_req;

    *child_required_props = NIL;
    MemSet(output, 0, sizeof(PgOutputProperty));

    switch (expr->op)
    {
        case PG_CASCADES_PHYSICAL_HASHAGG:
            child_req = pg_required_property_copy(ctx, required);
            child_req->pathkeys = NIL;
            if (required->required_columns != NULL)
                child_req->required_columns = bms_copy(required->required_columns);
            *child_required_props = list_make1(child_req);
            output->pathkeys = NIL;
            output->rows = ctx->upper->dNumGroups;
            break;

        case PG_CASCADES_PHYSICAL_GROUPAGG:
            child_req = pg_required_property_copy(ctx, required);
            child_req->pathkeys = ctx->upper->group_pathkeys;
            if (required->required_columns != NULL)
                child_req->required_columns = bms_copy(required->required_columns);
            *child_required_props = list_make1(child_req);
            output->pathkeys = ctx->upper->group_pathkeys;
            output->rows = ctx->upper->dNumGroups;
            break;

        case PG_CASCADES_PHYSICAL_SORT:
            child_req = pg_required_property_copy(ctx, required);
            child_req->pathkeys = NIL;
            if (required->required_columns != NULL)
                child_req->required_columns = bms_copy(required->required_columns);
            *child_required_props = list_make1(child_req);
            output->pathkeys = required->pathkeys;
            break;

        case PG_CASCADES_PHYSICAL_LIMIT:
            child_req = pg_required_property_copy(ctx, required);
            child_req->pathkeys = required->pathkeys;
            child_req->limit_tuples = ctx->upper->limit_tuples;
            if (required->required_columns != NULL)
                child_req->required_columns = bms_copy(required->required_columns);
            *child_required_props = list_make1(child_req);
            output->pathkeys = required->pathkeys;
            output->rows = ctx->upper->limit_tuples;
            break;

        case PG_CASCADES_PHYSICAL_UNIQUE:
            child_req = pg_required_property_copy(ctx, required);
            child_req->pathkeys = ctx->upper->distinct_pathkeys;
            if (required->required_columns != NULL)
                child_req->required_columns = bms_copy(required->required_columns);
            *child_required_props = list_make1(child_req);
            output->pathkeys = ctx->upper->distinct_pathkeys;
            output->rows = ctx->upper->dNumGroups;
            break;

        case PG_CASCADES_PHYSICAL_PROJECT:
            child_req = pg_required_property_copy(ctx, required);
            child_req->pathkeys = required->pathkeys;
            if (required->required_columns != NULL)
                child_req->required_columns = bms_copy(required->required_columns);
            *child_required_props = list_make1(child_req);
            output->pathkeys = NIL;  /* 保守：Project 不保证 pathkeys */
            break;

        /* IMPORTED_PATH mode: 从 Path * 读取 */
        case PG_CASCADES_PHYSICAL_SEQSCAN:
        case PG_CASCADES_PHYSICAL_INDEXSCAN:
        case PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN:
        case PG_CASCADES_PHYSICAL_NESTLOOP:
        case PG_CASCADES_PHYSICAL_HASHJOIN:
        case PG_CASCADES_PHYSICAL_MERGEJOIN:
            {
                Path *path = (Path *) expr->op_private;
                output->pathkeys = path->pathkeys;
                output->required_outer = path->param_info ?
                    path->param_info->ppi_req_outer : NULL;
                output->rows = path->parent->rows;
                output->width = path->parent->width;
            }
            break;

        default:
            elog(ERROR, "unexpected cascades op kind: %d", expr->op);
    }
}

/* ========================================================================
 * Group Best Entry 管理
 * ======================================================================== */

void
pg_group_update_best(PgMemoGroup *group, PgGroupBestEntry *new_entry)
{
    ListCell   *lc;

    foreach(lc, group->best_entries)
    {
        PgGroupBestEntry *old = (PgGroupBestEntry *) lfirst(lc);

        if (pg_required_property_equal(old->required, new_entry->required))
        {
            /* Replace in-place if new is cheaper */
            if (cascades_planner_debug)
                elog(NOTICE, "update_best: old_op=%d old_cost=%.4f new_op=%d new_cost=%.4f group=%d",
                     old->expr->op, old->total_cost,
                     new_entry->expr->op, new_entry->total_cost,
                     group->id);
            if (new_entry->total_cost < old->total_cost)
            {
                old->expr = new_entry->expr;
                old->startup_cost = new_entry->startup_cost;
                old->total_cost = new_entry->total_cost;
                old->child_required_props = new_entry->child_required_props;
                old->output = new_entry->output;
            }
            return;
        }
    }

    if (cascades_planner_debug)
        elog(NOTICE, "update_best: NEW entry op=%d cost=%.4f group=%d",
             new_entry->expr->op, new_entry->total_cost, group->id);

    /* New required property */
    group->best_entries = lappend(group->best_entries, new_entry);
}
