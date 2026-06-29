/*-------------------------------------------------------------------------
 * registry.c
 *    Cascades Module Registry — singleton for operator vtables + rules.
 *    Modules call pg_registry_register_*() at init time.
 *    Framework queries pg_registry_get_*() during optimization.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "optimizer/cascades.h"
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
    if (vt == NULL) return;
    if (vt->op >= 0 && vt->op < 64)
        g_registry.operators[vt->op] = vt;
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

    /* Insert in promise-descending order */
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

int
pg_registry_next_rule_bit(void)
{
    return g_registry.next_rule_bit++;
}
