/*-------------------------------------------------------------------------
 * registry.c
 *    Cascades Module Registry — singleton for operator vtables + rules.
 *    Modules call pg_registry_register_*() at init time.
 *    Framework queries pg_registry_get_*() during optimization.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "optimizer/cascades.h"
#include "core/registry.h"
#include "utils/memutils.h"

/* ── Singleton ── */
typedef struct PgCascadesRegistry {
    PgOperatorVtable *operators[64];      /* op_kind → vtable */
    List             *rules[3];           /* index by PgRuleCategory */
    int               next_rule_bit;
} PgCascadesRegistry;

static PgCascadesRegistry g_registry;

/* ── Init ── */
void
pg_registry_init(void)
{
    MemSet(&g_registry, 0, sizeof(PgCascadesRegistry));
}

/* ── Register operator vtable ── */
void
pg_registry_register_operator(PgOperatorVtable *vt)
{
    PgOperatorVtable *copy;
    if (vt == NULL) return;
    if (vt->op < 0 || vt->op >= 64) return;

    /* Copy to heap — caller's vtable is on the stack */
    copy = (PgOperatorVtable *) MemoryContextAllocZero(TopMemoryContext,
                                                        sizeof(PgOperatorVtable));
    memcpy(copy, vt, sizeof(PgOperatorVtable));
    g_registry.operators[vt->op] = copy;
}

/* ── Register rule (promise-sorted insertion) ── */
void
pg_registry_register_rule(PgRuleCategory cat,
    const char *name, PgCascadesOpKind from_op, PgCascadesOpKind to_op,
    void *pattern_or_null,
    List *(*transform)(PgPlannerCascadesContext *, PgGroupExpr *),
    double promise, int rule_bit)
{
    PgRule *rule;
    ListCell *lc;
    List     *new_list = NIL;
    bool      inserted = false;

    if (cat < 0 || cat > 2) return;

    rule = (PgRule *) MemoryContextAllocZero(TopMemoryContext, sizeof(PgRule));
    rule->name      = pstrdup(name);
    rule->from_op   = from_op;
    rule->to_op     = to_op;
    rule->pattern   = (struct Pattern *) pattern_or_null;
    rule->transform = transform;
    rule->promise   = promise;
    rule->rule_bit  = rule_bit;

    /* Set rule_type from category + from_op heuristic:
     * - from_op==0 and to_op!=0 → PG_RULE_ENFORCER (property enforcement)
     * - PG_RULE_MEMO_IMPL       → PG_RULE_IMPL
     * - PG_RULE_MEMO_TRANSFORM  → PG_RULE_TRANS
     * - PG_RULE_PRE_MEMO        → PG_RULE_TRANS (not used in task scheduler)
     */
    if (from_op == 0 && to_op != 0)
        rule->rule_type = PG_RULE_ENFORCER;
    else if (cat == PG_RULE_MEMO_IMPL)
        rule->rule_type = PG_RULE_IMPL;
    else
        rule->rule_type = PG_RULE_TRANS;

    /* Insert in promise-descending order.
     * CRITICAL: switch to TopMemoryContext so list nodes survive across
     * queries.  The rule objects themselves are already in TopMemoryContext;
     * lappend() uses palloc which allocates in the current context — if we
     * don't switch, list nodes end up in the first query's planner context
     * and become dangling pointers when that context is reset, causing
     * intermittent SIGSEGV on subsequent queries. */
    {
        MemoryContext old_cxt = MemoryContextSwitchTo(TopMemoryContext);

        foreach(lc, g_registry.rules[cat])
        {
            PgRule *r = (PgRule *) lfirst(lc);
            if (!inserted && promise > r->promise)
            {
                new_list = lappend(new_list, rule);
                inserted = true;
            }
            new_list = lappend(new_list, r);
        }
        if (!inserted)
            new_list = lappend(new_list, rule);

        MemoryContextSwitchTo(old_cxt);
    }
    g_registry.rules[cat] = new_list;
}

/* ── Query ── */
PgOperatorVtable *
pg_registry_get_vtable(PgCascadesOpKind op)
{
    if (op >= 0 && op < 64)
        return g_registry.operators[op];
    return NULL;
}

List *
pg_registry_get_rules(PgRuleCategory cat)
{
    if (cat >= 0 && cat <= 2)
        return g_registry.rules[cat];
    return NIL;
}

/* qsort comparator: promise descending (matches rule.c:pg_rule_promise_compare) */
static int
pg_registry_promise_compare(const void *a, const void *b)
{
    const PgRule *ra = (const PgRule *) a;
    const PgRule *rb = (const PgRule *) b;
    if (ra->promise > rb->promise) return -1;
    if (ra->promise < rb->promise) return 1;
    return 0;
}

/*
 * pg_registry_get_rules_array:
 *   Convert registry rules for a category into a sentinel-terminated array
 *   sorted by promise descending.
 *   The array is palloc'd; caller frees with pfree.
 */
PgRule *
pg_registry_get_rules_array(PgRuleCategory cat, int *num_rules)
{
    List     *rule_list;
    int       n, i;
    PgRule   *array;
    ListCell *lc;

    rule_list = g_registry.rules[cat];
    n = list_length(rule_list);

    if (n == 0)
    {
        *num_rules = 0;
        return NULL;
    }

    array = (PgRule *) palloc(sizeof(PgRule) * (n + 1));
    i = 0;
    foreach(lc, rule_list)
    {
        memcpy(&array[i], lfirst(lc), sizeof(PgRule));
        i++;
    }
    MemSet(&array[n], 0, sizeof(PgRule));  /* sentinel */

    /* Sort by promise descending (matching old pg_cascades_get_rules_sorted) */
    qsort(array, n, sizeof(PgRule), pg_registry_promise_compare);

    *num_rules = n;
    return array;
}

/*
 * Backward-compatible wrappers for old rule.c API.
 * rewrite.c uses these to look up transform functions by name.
 */

/* pg_cascades_get_trans_rules_phase5 — return all PG_RULE_MEMO_TRANSFORM rules */
PgRule *
pg_cascades_get_trans_rules_phase5(int *num_rules)
{
    return pg_registry_get_rules_array(PG_RULE_MEMO_TRANSFORM, num_rules);
}

/* pg_cascades_get_trans_rules — same bucket (old phase3+5 distinction merged) */
PgRule *
pg_cascades_get_trans_rules(int *num_rules)
{
    return pg_registry_get_rules_array(PG_RULE_MEMO_TRANSFORM, num_rules);
}

int
pg_registry_next_rule_bit(void)
{
    return g_registry.next_rule_bit++;
}
