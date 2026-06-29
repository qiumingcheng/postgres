/*-------------------------------------------------------------------------
 * registry.h
 *    Cascades Module Registry — operator vtable + rule registration API.
 *    Modules call pg_registry_register_*() at init time.
 *    Framework queries pg_registry_get_*() during optimization.
 *    Self-contained: includes cascades.h for base types.
 *-------------------------------------------------------------------------
 */
#ifndef CASCADES_REGISTRY_H
#define CASCADES_REGISTRY_H

#include "optimizer/cascades.h"

/* ── Rule categories (3 buckets) ── */
typedef enum PgRuleCategory {
    PG_RULE_PRE_MEMO,          /* pre-Memo rules (g_pre_memo_rules) */
    PG_RULE_MEMO_TRANSFORM,    /* Memo transform rules (Phase 3/5) */
    PG_RULE_MEMO_IMPL,         /* Memo impl rules (Phase 1/2/4) */
} PgRuleCategory;

/* ── Vtable callback signatures ── */
typedef void (*PgCostFn)(PgPlannerCascadesContext *ctx,
    PgMemoGroup *group, PgGroupExpr *expr,
    double input_rows, int input_width,
    Cost child_startup, Cost child_total,
    Cost *out_startup, Cost *out_total);

typedef Plan *(*PgBuildPlanFn)(PgPlannerCascadesContext *ctx,
    PgMemoGroup *group, PgGroupExpr *expr,
    PgGroupBestEntry *best, PgRequiredProperty *required,
    PgOutputProperty *output);

typedef void (*PgDeriveStatsFn)(PgPlannerCascadesContext *ctx,
    PgGroupExpr *expr, double *out_rows, int *out_width);

/* ── Operator Vtable ── */
typedef struct PgOperatorVtable {
    PgCascadesOpKind   op;
    const char        *name;
    PgCostFn           cost_fn;
    PgBuildPlanFn      build_plan_fn;
    PgDeriveStatsFn    derive_stats_fn;
} PgOperatorVtable;

/* ── Registry API ── */
extern void pg_registry_init(void);
extern void pg_registry_register_operator(PgOperatorVtable *vt);
extern void pg_registry_register_rule(PgRuleCategory cat,
    const char *name, PgCascadesOpKind from_op, PgCascadesOpKind to_op,
    void *pattern_or_null,
    List *(*transform)(PgPlannerCascadesContext *, PgGroupExpr *),
    double promise, int rule_bit);
extern PgOperatorVtable *pg_registry_get_vtable(PgCascadesOpKind op);
extern List *pg_registry_get_rules(PgRuleCategory cat);
extern PgRule *pg_registry_get_rules_array(PgRuleCategory cat, int *num_rules);
extern int  pg_registry_next_rule_bit(void);

#endif /* CASCADES_REGISTRY_H */
