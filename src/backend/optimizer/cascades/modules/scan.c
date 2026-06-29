#include "postgres.h"
#include "optimizer/cascades.h"
#include "core/registry.h"

/* === pg_rule_scan_to_seqscan (moved from rule.c) === */

List *
pg_rule_scan_to_seqscan(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_SEQSCAN);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = NIL;
    result->op_private = expr->op_private;
    return list_make1(result);
}


/* === pg_rule_scan_to_indexscan (moved from rule.c) === */

List *
pg_rule_scan_to_indexscan(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_INDEXSCAN);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = NIL;
    result->op_private = expr->op_private;
    return list_make1(result);
}


/* === pg_rule_scan_to_bitmapheapscan (moved from rule.c) === */

List *
pg_rule_scan_to_bitmapheapscan(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgGroupExpr *result = pg_memo_new_group_expr(ctx,
                                                  PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN);
    result->mode = PG_PHYS_EXPR_COMPOSABLE_OP;
    result->inputs = NIL;
    result->op_private = expr->op_private;
    return list_make1(result);
}


/* === pg_rule_prune_empty_scan (moved from rule.c) === */

List *
pg_rule_prune_empty_scan(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    RelOptInfo *rel = expr->owner_group->rel;

    if (rel == NULL || rel->rows > 0)
        return NIL;

    /* 标记 group 为空（让后续 E1 PruneEmptyJoin 能检测到） */
    expr->owner_group->rows = 0;
    expr->owner_group->width = 0;
    return NIL;
}


/* === pg_rule_pushdown_predicate_scan (moved from rule.c) === */

List *
pg_rule_pushdown_predicate_scan(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    PgMemoGroup *child_group;
    List        *filter_quals = (List *) expr->op_private;
    RelOptInfo  *rel;
    ListCell    *lc;
    List        *pushable = NIL;
    List        *remain   = NIL;

    if (expr->inputs == NIL)
        return NIL;

    child_group = (PgMemoGroup *) linitial(expr->inputs);
    rel = child_group->rel;

    if (rel == NULL || filter_quals == NIL)
        return NIL;

    foreach(lc, filter_quals)
    {
        RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

        /* volatile → 不推 */
        if (contain_volatile_functions((Node *) ri->clause))
        {
            remain = lappend(remain, ri);
            continue;
        }

        /* qual 涉及 scan 之外的 rel → 不推 */
        if (!bms_is_subset(ri->clause_relids, rel->relids))
        {
            remain = lappend(remain, ri);
            continue;
        }

        pushable = lappend(pushable, ri);
    }

    if (pushable == NIL)
        return NIL;

    /* 将可推 qual 合并到 baserestrictinfo */
    rel->baserestrictinfo = list_concat(rel->baserestrictinfo, pushable);

    if (remain == NIL)
    {
        /* 所有 qual 都推完了 → 返回裸 LogicalScan */
        PgGroupExpr *scan_logical = pg_memo_group_first_logical(child_group,
                                        PG_CASCADES_LOGICAL_SCAN);
        PgGroupExpr *new_scan = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_SCAN);
        new_scan->inputs = NIL;
        new_scan->op_private = (scan_logical != NULL) ?
            scan_logical->op_private : NULL;
        return list_make1(new_scan);
    }
    else
    {
        /* 还有 qual → 返回变薄的 LogicalFilter */
        PgGroupExpr *new_filter = pg_memo_new_group_expr(ctx, PG_CASCADES_LOGICAL_FILTER);
        new_filter->inputs = expr->inputs;  /* child 不变 */
        new_filter->op_private = remain;
        return list_make1(new_filter);
    }
}


/* === pg_rule_prune_scan_columns (moved from rule.c) === */

List *
pg_rule_prune_scan_columns(PgPlannerCascadesContext *ctx, PgGroupExpr *expr)
{
    RelOptInfo *rel = expr->owner_group->rel;
    ListCell   *lc;
    Bitmapset  *needed = NULL;
    List       *new_tlist = NIL;
    bool        pruned = false;

    if (rel == NULL)
        return NIL;

    /* Collect columns from baserestrictinfo (quals reference these) */
    foreach(lc, rel->baserestrictinfo)
    {
        RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
        pull_varattnos((Node *) ri->clause, rel->relid, &needed);
    }

    /* Always keep columns referenced by the target list (upper->tlist) */
    if (ctx->upper != NULL && ctx->upper->tlist != NIL)
    {
        pull_varattnos((Node *) ctx->upper->tlist, rel->relid, &needed);
    }

    /* Also collect from logical_prop.output_columns (B2-B5 propagation) */
    if (expr->owner_group->logical_prop.output_columns != NULL)
    {
        needed = bms_union(needed,
                    bms_copy(expr->owner_group->logical_prop.output_columns));
    }

    /* Also collect from best entries' required_columns (parent projections) */
    foreach(lc, expr->owner_group->best_entries)
    {
        PgGroupBestEntry *entry = (PgGroupBestEntry *) lfirst(lc);
        if (entry->required != NULL && entry->required->required_columns != NULL)
        {
            needed = bms_union(needed, entry->required->required_columns);
        }
    }

    /*
     * Safety check for multi-table joins with ORDER BY + LIMIT:
     * Disable column pruning entirely to avoid missing intermediate join keys.
     * This is a conservative approach for the edge case of 3+ table joins.
     */
    if (rel->has_eclass_joins &&
        ctx->root->parse->sortClause != NIL &&
        ctx->root->parse->limitCount != NULL)
    {
        int num_rels = bms_num_members(ctx->root->all_baserels);

        if (num_rels >= 3)
        {
            CASCADES_DEBUG(ctx->debug, "Cascades: PruneScanColumns disabled for "
                     "multi-table join (n=%d) with ORDER+LIMIT to preserve join keys",
                     num_rels);

            bms_free(needed);
            return NIL;
        }
    }

    if (needed == NULL)
    {
        bms_free(needed);
        return NIL;
    }

    /* Build pruned reltargetlist: keep only vars in needed */
    {
        bool any_attr_needed = false;

        foreach(lc, rel->reltargetlist)
        {
            Var *var = (Var *) lfirst(lc);
            int ndx = var->varattno - rel->min_attr;

            /*
             * Phase 6: Always keep columns that the standard planner marked
             * as needed (attr_needed bitmap non-empty).  These columns are
             * required by join quals at higher levels.  Track whether any
             * column has attr_needed set — if none do, the requirements
             * are incomplete and pruning is unsafe.
             */
            if (ndx >= 0 && ndx < rel->max_attr - rel->min_attr + 1)
            {
                if (!bms_is_empty(rel->attr_needed[ndx]))
                {
                    any_attr_needed = true;
                    CASCADES_DEBUG(ctx->debug, "Cascades: PruneScanColumns keeping varno=%d varattno=%d — attr_needed",
                             (int)var->varno, (int)var->varattno);
                    new_tlist = lappend(new_tlist, var);
                    continue;  /* keep: needed by some join */
                }
            }

            if (bms_is_member(var->varattno, needed))
                new_tlist = lappend(new_tlist, var);
            else
                pruned = true;
        }

        /*
         * Safety: if NO column had attr_needed set, PG hasn't confirmed
         * which columns are join-needed.  Pruning in this state can remove
         * columns that upper plan nodes still reference, causing
         * "variable not found in subplan target lists" errors.
         * Skip pruning entirely — even if output_columns exist, they
         * may be incomplete (missing join keys from other tables).
         */
        if (!any_attr_needed)
        {
            list_free(new_tlist);
            bms_free(needed);
            return NIL;
        }

        /*
         * Additional safety: disable column pruning for scans that feed
         * into multi-table joins (3+ tables) with ORDER BY + LIMIT.
         * These scenarios are prone to missing intermediate join keys.
         *
         * Detection heuristic: if this relation participates in joins
         * and the query has both ORDER BY and LIMIT, be conservative.
         */
        if (rel->has_eclass_joins &&
            ctx->root->parse->sortClause != NIL &&
            ctx->root->parse->limitCount != NULL)
        {
            /* Count number of joined relations */
            int num_rels = bms_num_members(ctx->root->all_baserels);

            if (num_rels >= 3 && pruned)
            {
                CASCADES_DEBUG(ctx->debug, "Cascades: PruneScanColumns skipping pruning for "
                         "multi-table join (n=%d) with ORDER+LIMIT", num_rels);

                list_free(new_tlist);
                bms_free(needed);
                return NIL;
            }
        }
    }

    /*
     * Phase 6: Safety check — never reduce to 0 columns.
     * A scan producing no columns breaks PG's create_plan and executor.
     */
    if (pruned && list_length(new_tlist) > 0)
    {
        rel->reltargetlist = new_tlist;
        /* Re-estimate: fewer columns → narrower rows */
        set_baserel_size_estimates(ctx->root, rel);

        CASCADES_DEBUG(ctx->debug, "Cascades: PruneScanColumns reduced reltargetlist "
                 "for rel %d to %d columns",
                 rel->relid, list_length(new_tlist));
    }

    bms_free(needed);
    return NIL;  /* side-effect only */
}

void pg_module_scan_init(void) {
    pg_registry_register_operator(&(PgOperatorVtable){.op=PG_CASCADES_PHYSICAL_SEQSCAN,.name="SeqScan"});
    pg_registry_register_operator(&(PgOperatorVtable){.op=PG_CASCADES_PHYSICAL_INDEXSCAN,.name="IndexScan"});
    pg_registry_register_operator(&(PgOperatorVtable){.op=PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN,.name="BitmapHeapScan"});
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,"LogicalScan→PhysicalSeqScan",PG_CASCADES_LOGICAL_SCAN,PG_CASCADES_PHYSICAL_SEQSCAN,NULL,pg_rule_scan_to_seqscan,0.5,PG_RULE_BIT_SCAN_TO_SEQSCAN);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,"LogicalScan→PhysicalIndexScan",PG_CASCADES_LOGICAL_SCAN,PG_CASCADES_PHYSICAL_INDEXSCAN,NULL,pg_rule_scan_to_indexscan,0.8,PG_RULE_BIT_SCAN_TO_INDEXSCAN);
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,"LogicalScan→PhysicalBitmapHeapScan",PG_CASCADES_LOGICAL_SCAN,PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN,NULL,pg_rule_scan_to_bitmapheapscan,0.7,PG_RULE_BIT_SCAN_TO_BITMAPSCAN);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"PruneEmptyScan",PG_CASCADES_LOGICAL_SCAN,0,NULL,pg_rule_prune_empty_scan,0.9,PG_RULE_BIT_PRUNE_EMPTY_SCAN);
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,"PruneScanColumns",PG_CASCADES_LOGICAL_SCAN,0,NULL,pg_rule_prune_scan_columns,0.6,PG_RULE_BIT_PRUNE_SCAN_COLS);
}
