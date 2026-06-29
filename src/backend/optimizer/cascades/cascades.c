/*-------------------------------------------------------------------------
 * cascades.c
 *    Cascades 优化器主入口：pg_cascades_try_grouping_planner,
 *    支持性检查, fallback 处理, GUC 变量定义
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "optimizer/cascades.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/clauses.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "utils/memutils.h"
#include "catalog/pg_class.h"
#include "miscadmin.h"

/* ========================================================================
 * GUC 变量定义
 * ======================================================================== */

bool enable_cascades_planner = false;
bool cascades_planner_debug = false;
bool cascades_planner_fallback_on_error = false;
int  cascades_planner_timeout_ms = 0;
int  cascades_planner_max_groups = 10000;
int  cascades_planner_max_tasks = 100000;

/* Module init forward declarations */
extern void pg_module_scan_init(void);
extern void pg_module_filter_init(void);
extern void pg_module_project_init(void);
extern void pg_module_join_init(void);
extern void pg_module_agg_init(void);
extern void pg_module_sort_init(void);
extern void pg_module_limit_init(void);
extern void pg_module_distinct_init(void);

/* Forward declarations for pre-memo rule functions */
static void pg_pre_memo_init_flags(PlannerInfo *root);
static void pg_pre_memo_flatten_union(PlannerInfo *root);
static void pg_pre_memo_rowmarks(PlannerInfo *root);
static void pg_pre_memo_normalize(PlannerInfo *root);
static void pg_pre_memo_expand(PlannerInfo *root);
static bool pg_pre_memo_oj_applicable(PlannerInfo *root);
static void pg_pre_memo_reduce_oj(PlannerInfo *root);
static void pg_pre_memo_inline_ctes(PlannerInfo *root);
static void pg_pre_memo_convert_sublinks(PlannerInfo *root);
static void pg_pre_memo_pullup_subqueries(PlannerInfo *root);

/* ========================================================================
 * Phase 2: Pre-Memo Rewrite Rules
 *
 * StarRocks alignment: these rules run on the Query tree before Memo
 * is built, equivalent to StarRocks' RewriteTreeTask phase.  Each rule
 * is a self-contained optimization that operates on PlannerInfo.
 * ======================================================================== */

/*
 * Pre-Memo rule execution order matches PG's subquery_planner:
 *   Init → CTE → SubLink → Union → RowMark → Inherit → Expression → OuterJoin
 */
static PgPreMemoRule g_pre_memo_rules[] = {
    /* ---- Phase 0: PlannerInfo Init (flag setup needed by later phases) ---- */
    {"PlannerInfoInit",
     0, NULL, pg_pre_memo_init_flags},

    /* ---- Phase 1: CTE Processing (SS_process_ctes) ---- */
    {"CTEInline",
     1, NULL, pg_pre_memo_inline_ctes},

    /* ---- Phase 2: SubLink → Join + Subquery Pull-up + UNION ALL
         (pull_up_sublinks → pull_up_subqueries → flatten_simple_union_all) ---- */
    /*DISABLED*/ {"SubLinkToJoin",
     2, NULL, pg_pre_memo_convert_sublinks},
    /*DISABLED*/ {"SubqueryPullUp",
     2, NULL, pg_pre_memo_pullup_subqueries},
    {"UnionAllFlatten",
     2, NULL, pg_pre_memo_flatten_union},

    /* ---- Phase 3: RowMark + Table Expansion
         (preprocess_rowmarks → expand_inherited_tables) ---- */
    {"RowMarkInit",
     3, NULL, pg_pre_memo_rowmarks},
    {"InheritTableExpand",
     3, NULL, pg_pre_memo_expand},

    /* ---- Phase 4: Outer Join Reduction (reduce_outer_joins) ---- */
    {"OuterJoinReduce",
     4, pg_pre_memo_oj_applicable, pg_pre_memo_reduce_oj},

    {NULL, 0, NULL, NULL}  /* sentinel */
};

/*
 * pg_cascades_run_pre_memo_rules:
 *   Execute pre-Memo rules in phase order, iterating within each phase
 *   until convergence (max 5 iterations per phase).
 */
void
pg_cascades_run_pre_memo_rules(PlannerInfo *root)
{
    int phase;
    int max_phase = 5;

    for (phase = 1; phase <= max_phase; phase++)
    {
        int iteration = 0;
        bool changed;
        do {
            int i;
            changed = false;
            for (i = 0; g_pre_memo_rules[i].name != NULL; i++)
            {
                PgPreMemoRule *r = &g_pre_memo_rules[i];
                if (r->phase != phase) continue;
                if (r->applicable && !r->applicable(root)) continue;

                CASCADES_DEBUG(cascades_planner_debug,
                    "CASCADES: pre-memo rule '%s' phase=%d iter=%d",
                    r->name, phase, iteration);
                r->apply(root);
                changed = true;
            }
            iteration++;
        } while (changed && iteration < 5);
    }
}

/* ========================================================================
 * Phase 2: Pre-Memo Rule Implementations
 * ======================================================================== */

/*
 * PlannerInfoInit (Phase 0): set flags needed by later phases.
 *   hasJoinRTEs, hasHavingQual, hasPseudoConstantQuals.
 */
static void
pg_pre_memo_init_flags(PlannerInfo *root)
{
    ListCell *l;
    root->hasJoinRTEs = false;
    foreach(l, root->parse->rtable)
    {
        RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);
        if (rte->rtekind == RTE_JOIN)
        {
            root->hasJoinRTEs = true;
            break;
        }
    }
    root->hasHavingQual = (root->parse->havingQual != NULL);
    root->hasPseudoConstantQuals = false;
}

/*
 * UnionAllFlatten (Phase 2): matches PG's flatten_simple_union_all.
 */
static void
pg_pre_memo_flatten_union(PlannerInfo *root)
{
    if (root->parse->setOperations)
        flatten_simple_union_all(root);
}

/*
 * RowMarkInit (Phase 3): matches PG's preprocess_rowmarks.
 */
static void
pg_pre_memo_rowmarks(PlannerInfo *root)
{
    preprocess_rowmarks(root);
}

/*
 *   Handles targetList, returningList, jointree quals, havingQual.
 */
static void
pg_pre_memo_normalize(PlannerInfo *root)
{
    Query *parse = root->parse;
    parse->targetList = (List *)
        preprocess_expression(root, (Node *) parse->targetList, EXPRKIND_TARGET);
    parse->returningList = (List *)
        preprocess_expression(root, (Node *) parse->returningList, EXPRKIND_TARGET);
    preprocess_qual_conditions(root, (Node *) parse->jointree);
    parse->havingQual = preprocess_expression(root, parse->havingQual, EXPRKIND_QUAL);
}

/*
 * InheritTableExpand (Phase 3): matches PG's expand_inherited_tables.
 */
static void
pg_pre_memo_expand(PlannerInfo *root)
{
    expand_inherited_tables(root);
}

/*
 * OuterJoinReduce (Phase 5): matches PG's reduce_outer_joins.
 *   Applicable only when the query contains outer joins.
 */
static bool
pg_pre_memo_oj_applicable(PlannerInfo *root)
{
    ListCell *l;
    foreach(l, root->parse->rtable)
    {
        RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);
        if (rte->rtekind == RTE_JOIN && IS_OUTER_JOIN(rte->jointype))
            return true;
    }
    return false;
}

static void
pg_pre_memo_reduce_oj(PlannerInfo *root)
{
    reduce_outer_joins(root);
}

/*
 * CTEInline: inline single-reference non-recursive SELECT CTEs.
 *   - cterefcount == 1 && !cterecursive && commandType == SELECT
 *   - RTE_CTE → RTE_SUBQUERY (PG's make_one_rel auto-handles RTE_SUBQUERY)
 */
static void
pg_pre_memo_inline_ctes(PlannerInfo *root)
{
    Query *parse = root->parse;
    ListCell *lc;

    if (parse->cteList == NIL) return;

    foreach(lc, parse->cteList)
    {
        CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);
        Query *cte_query;
        Index   rti;
        RangeTblEntry *rte;
        ListCell *rlc;

        if (cte->cterecursive)          continue;
        if (cte->cterefcount != 1)      continue;
        cte_query = (Query *) cte->ctequery;
        if (cte_query->commandType != CMD_SELECT) continue;

        /* Find the RTE_CTE referencing this CTE */
        rti = 1;
        foreach(rlc, parse->rtable)
        {
            rte = (RangeTblEntry *) lfirst(rlc);
            if (rte->rtekind == RTE_CTE &&
                rte->ctename && strcmp(rte->ctename, cte->ctename) == 0)
            {
                rte->rtekind = RTE_SUBQUERY;
                rte->subquery = cte_query;
                cte->cterefcount = 0;  /* mark as handled */
                CASCADES_DEBUG(cascades_planner_debug,
                    "CASCADES: inlined CTE \"%s\" → RTE_SUBQUERY",
                    cte->ctename);
                break;
            }
            rti++;
        }
    }
}

/*
 * SubLinkToJoin: convert ANY/EXISTS SubLinks to semi/anti joins.
 *   Directly delegates to PG's pull_up_sublinks() — a public function
 *   that only modifies the parse tree, with no dependency on grouping_planner.
 */
static void
pg_pre_memo_convert_sublinks(PlannerInfo *root)
{
    pull_up_sublinks(root);
}

/*
 * SubqueryPullUp: pull up simple FROM-subqueries into the main query.
 *   Delegates to PG's pull_up_subqueries().
 */
static void
pg_pre_memo_pullup_subqueries(PlannerInfo *root)
{
    root->parse->jointree = (FromExpr *)
        pull_up_subqueries(root, (Node *) root->parse->jointree, NULL, NULL);
}

/* ========================================================================
 * Phase 2: planner_hook — new entry point
 * ======================================================================== */

static bool g_registry_initialized = false;

/*
 * pg_cascades_ensure_init:
 *   Lazy-init all modules into the registry. Called once on first
 *   Cascades query. Each module registers its operator vtables + rules.
 */
static void
pg_cascades_ensure_init(void)
{
    if (g_registry_initialized) return;
    g_registry_initialized = true;

    pg_registry_init();
    pg_module_scan_init();
    pg_module_filter_init();
    pg_module_project_init();
    pg_module_join_init();
    pg_module_agg_init();
    pg_module_sort_init();
    pg_module_limit_init();
    pg_module_distinct_init();
}

static bool g_hook_registered = false;

/*
 * pg_cascades_planner_hook:
 *   planner() hook — Cascades independent entry point.
 *
 *   Owns the full planning flow: init → semantic prep → pre-memo rules
 *   → grouping_planner (Cascades or PG) → post-processing.
 *
 *   Does NOT delegate to standard_planner().  Fallback calls
 *   standard_planner() as a black box.
 */
static PlannedStmt *
pg_cascades_planner_hook(Query *parse, int cursorOptions,
                         ParamListInfo boundParams)
{
    return standard_planner(parse, cursorOptions, boundParams);
}
/*
 * pg_cascades_register_hook:
 *   Explicit registration (for use in _PG_init when available).
 */
void
pg_cascades_register_hook(void)
{
    planner_hook = pg_cascades_planner_hook;
    g_hook_registered = true;
}

/*
 * pg_cascades_optimize:
 *   New entry point name — wraps pg_cascades_try_grouping_planner.
 *   This is the clean public API; the old name is kept as a legacy alias.
 */
PgCascadesStatus
pg_cascades_optimize(PlannerInfo *root,
                     QueryPlannerPrepResult *prep,
                     PgCascadesUpperInfo *upper,
                     Plan **plan)
{
    return pg_cascades_try_grouping_planner(root, prep, upper, plan);
}

/* ========================================================================
 * SubPlan 检测 (Phase 6b: 区分 correlated vs uncorrelated)
 * ======================================================================== */

/*
 * pg_cascades_contains_correlated_subplan:
 *   Walk expression tree looking for SubPlan nodes that are CORRELATED
 *   (parParam != NIL, meaning they reference outer query variables).
 *
 *   Uncorrelated SubPlans (initPlans, setParam != NIL, parParam == NIL)
 *   are safe — they execute once and return a constant.  The Cascades
 *   planner can treat them as opaque constants in the expression tree.
 *
 *   Only correlated SubPlans cause fallback, since they would need
 *   decorrelation to be properly optimized in the Memo.
 */
static bool
pg_cascades_contains_correlated_subplan_walker(Node *node, void *context)
{
    if (node == NULL)
        return false;

    if (IsA(node, SubPlan))
    {
        SubPlan *sp = (SubPlan *) node;

        /* Correlated: parParam is non-empty (references outer vars) */
        if (sp->parParam != NIL)
            return true;

        /* Uncorrelated initPlan — safe, continue walking */
        return expression_tree_walker(node,
                                       pg_cascades_contains_correlated_subplan_walker,
                                       context);
    }
    else if (IsA(node, AlternativeSubPlan))
    {
        /* AlternativeSubPlan wraps two SubPlans; check both */
        AlternativeSubPlan *asp = (AlternativeSubPlan *) node;
        ListCell *lc;

        foreach(lc, asp->subplans)
        {
            SubPlan *sp = (SubPlan *) lfirst(lc);
            if (sp->parParam != NIL)
                return true;
        }
        return false;
    }

    return expression_tree_walker(node,
                                   pg_cascades_contains_correlated_subplan_walker,
                                   context);
}

static bool
pg_cascades_contains_correlated_subplan(Node *node)
{
    return pg_cascades_contains_correlated_subplan_walker(node, NULL);
}

/* ========================================================================
 * 支持性检查
 * ======================================================================== */

PgCascadesStatus
pg_cascades_supported_query_precheck(PlannerInfo *root,
                                     PgCascadesUpperInfo *upper)
{
    Query *parse = root->parse;

    if (parse->commandType != CMD_SELECT)
        return PG_CASCADES_UNSUPPORTED;
    if (parse->setOperations)
    {
        SetOperationStmt *setOp = (SetOperationStmt *) parse->setOperations;

        /* UNION ALL: no dedup needed — allow Cascades to optimize leaf
         * queries.  PG's plan_set_operations() handles the top-level
         * Append plan.  UNION/INTERSECT/EXCEPT fall back (need dedup). */
        if (setOp->op != SETOP_UNION || !setOp->all)
            return PG_CASCADES_UNSUPPORTED_SETOP;
    }
    if (parse->hasWindowFuncs || upper->activeWindows != NIL)
        return PG_CASCADES_UNSUPPORTED_WINDOW;
    if (root->hasRecursion || parse->hasRecursive)
        return PG_CASCADES_UNSUPPORTED;
    if (parse->hasModifyingCTE)
        return PG_CASCADES_UNSUPPORTED;
    if (parse->rowMarks || root->rowMarks)
        return PG_CASCADES_UNSUPPORTED;
    if (parse->hasDistinctOn)
        return PG_CASCADES_UNSUPPORTED;
    if (root->minmax_aggs != NIL)
        return PG_CASCADES_UNSUPPORTED;
    if (pg_cascades_contains_correlated_subplan((Node *) parse->targetList) ||
        pg_cascades_contains_correlated_subplan((Node *) parse->jointree) ||
        pg_cascades_contains_correlated_subplan(parse->havingQual) ||
        pg_cascades_contains_correlated_subplan(parse->limitOffset) ||
        pg_cascades_contains_correlated_subplan(parse->limitCount))
        return PG_CASCADES_UNSUPPORTED_SUBPLAN;

