# 核心重构总结：
规则四分类：LOGICAL_TRANSFORM / LOGICAL_DECOMPOSE / PHYSICAL_IMPL / PHYSICAL_REWRITE——覆盖从逻辑到物理全链路
代价函数独立注册：不在 vtable 里，而在注册中心 cost_fns[op_kind] 中。同算子可注册多个代价模型（"default" / "gpu" / "distributed"），按 priority 竞争
Vtable 只放一一对应关系（build_plan / derive_stats）；注册中心放多对多关系（规则 / 代价函数）
新模块 = 一个文件：spatial_join.c，注册算子 → 注册代价 → 注册逻辑规则 → 注册分解规则 → 注册物理规则。不动任何框架代码
净减 ~1630 行：框架 switch-case 全部消除，规则分散到模块文件中


# 目录结构:
src/backend/optimizer/cascades/
│
├── core/                          ← 框架核心（不常改）
│   ├── registry.c                 ★ 新增：注册表 + API
│   ├── memo.c                     vtable 分发
│   ├── task.c                     vtable 分发
│   ├── planbuild.c                vtable 分发
│   ├── pattern.c                  不变
│   ├── property.c                 不变
│   └── rewrite.c                  调用 pre_memo_rules 列表
│
├── modules/                       ← 算子模块（加新功能只改这里）
│   ├── scan.c                     Scan 算子（逻辑+物理）
│   ├── filter.c                   Filter 算子
│   ├── project.c                  Project 算子
│   ├── join.c                     Join 三兄弟（NestLoop, Hash, Merge）
│   ├── agg.c                      Agg 算子（HashAgg, GroupAgg）
│   ├── sort.c                     Sort 算子
│   ├── limit.c                    Limit 算子
│   ├── distinct.c                 Distinct 算子
│   ├── union.c                    Union 算子
│
├── adapters/
│   ├── pg_adapter.c               PG 查询树 → OptExpression
│   ├── pg_plan.c                  PG Plan 构建工具
│   └── pg_stats.c                 PG 统计工具
│
├── cascades.c                     主入口 + hook
├── debug.c                        调试
└── postopt.c                      Plan 后优化
					
# 详细实施步骤
Step 1: 新增注册中心
1.1 创建 src/backend/optimizer/cascades/core/registry.h

/*-------------------------------------------------------------------------
 * registry.h
 *    Cascades 模块注册中心 — operator vtable + rule 三桶
 *-------------------------------------------------------------------------
 */
#ifndef CASCADES_REGISTRY_H
#define CASCADES_REGISTRY_H

#include "optimizer/cascades.h"

/* ── 规则三分类（仅覆盖现有规则）── */
typedef enum PgRuleCategory {
    PG_RULE_PRE_MEMO,          /* 现有 g_pre_memo_rules[]     → cascades.c */
    PG_RULE_MEMO_TRANSFORM,    /* 现有 g_trans_rules_phase3/5  → rule.c */
    PG_RULE_MEMO_IMPL,         /* 现有 g_impl_rules_phase1/2/4 → rule.c */
} PgRuleCategory;

/* ── 回调函数指针类型 ── */
typedef void (*PgCostFn)(PgPlannerCascadesContext *ctx,
                          PgMemoGroup *group, PgGroupExpr *expr,
                          double input_rows, int input_width,
                          Cost child_startup, Cost child_total,
                          Cost *out_startup, Cost *out_total);

typedef Plan *(*PgBuildPlanFn)(PgPlannerCascadesContext *ctx,
                                PgMemoGroup *group, PgGroupExpr *expr,
                                PgGroupBestEntry *best,
                                PgRequiredProperty *required,
                                PgOutputProperty *output);

typedef void (*PgDeriveStatsFn)(PgPlannerCascadesContext *ctx,
                                 PgGroupExpr *expr,
                                 double *out_rows, int *out_width);

/* ── Operator Vtable ── */
typedef struct PgOperatorVtable {
    PgCascadesOpKind   op;              /* 算子枚举值 */
    const char        *name;            /* "NestLoop", "HashAgg", ... */
    PgCostFn           cost_fn;         /* NULL → 默认代价 0.01 */
    PgBuildPlanFn      build_plan_fn;   /* NULL → IMPORTED_PATH 委托 */
    PgDeriveStatsFn    derive_stats_fn; /* NULL → 默认推导 */
} PgOperatorVtable;

/* ── 注册 API ── */
void pg_registry_init(void);
void pg_registry_register_operator(PgOperatorVtable *vt);
void pg_registry_register_rule(PgRuleCategory cat,
    const char *name, PgCascadesOpKind from_op, PgCascadesOpKind to_op,
    struct Pattern *pattern,
    List *(*transform)(PgPlannerCascadesContext *, PgGroupExpr *),
    double promise, int rule_bit);

/* 查询 */
PgOperatorVtable *pg_registry_get_vtable(PgCascadesOpKind op);
List             *pg_registry_get_rules(PgRuleCategory cat);
int               pg_registry_next_rule_bit(void);

#endif /* CASCADES_REGISTRY_H */
1.2 创建 src/backend/optimizer/cascades/core/registry.c

/*-------------------------------------------------------------------------
 * registry.c
 *    Cascades 全局注册表 — 单例，算子 + 规则集中管理
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "optimizer/cascades.h"
#include "core/registry.h"

/* ── 单例 ── */
typedef struct PgCascadesRegistry {
    PgOperatorVtable *operators[64];      /* op_kind → vtable */
    List             *rules[3];           /* 按 PgRuleCategory 索引 */
    int               next_rule_bit;      /* 自增 bit 分配 */
} PgCascadesRegistry;

static PgCascadesRegistry g_registry;

/* ── 初始化 ── */
void
pg_registry_init(void)
{
    MemSet(&g_registry, 0, sizeof(PgCascadesRegistry));
}

/* ── 注册算子 vtable ── */
void
pg_registry_register_operator(PgOperatorVtable *vt)
{
    if (vt == NULL) return;
    if (vt->op >= 0 && vt->op < 64)
        g_registry.operators[vt->op] = vt;
}

/* ── 注册规则 ── */
void
pg_registry_register_rule(PgRuleCategory cat,
    const char *name, PgCascadesOpKind from_op, PgCascadesOpKind to_op,
    Pattern *pattern,
    List *(*transform)(PgPlannerCascadesContext *, PgGroupExpr *),
    double promise, int rule_bit)
{
    PgRule *rule;

    if (cat < 0 || cat > 2) return;

    rule = (PgRule *) MemoryContextAllocZero(TopMemoryContext, sizeof(PgRule));
    rule->name      = pstrdup(name);
    rule->from_op   = from_op;
    rule->to_op     = to_op;
    rule->pattern   = pattern;
    rule->transform = transform;
    rule->promise   = promise;
    rule->rule_bit  = rule_bit;

    /* 按 promise 降序插入 */
    {
        ListCell *lc;
        List     *new_list = NIL;
        bool       inserted = false;

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
}

/* ── 查询 ── */
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
1.3 修改 src/backend/optimizer/cascades/Makefile

OBJS = cascades.o memo.o task.o rule.o property.o pattern.o \
       pg_adapter.o planbuild.o debug.o postopt.o rewrite.o \
       core/registry.o                                     ← 加这一行
1.4 验证

make -C src/backend/optimizer/cascades -j$(nproc)
# 预期: 编译通过，无错误（registry.c 编译后暂无人调用）
Step 2: cascades.h 追加声明
2.1 cascades.h 末尾追加
在文件末尾 #endif 前追加：


/* ── 模块化注册 API ── */
#include "core/registry.h"
2.2 验证

make -C src/backend/optimizer/cascades -j$(nproc)
# cascades.h 被所有文件 include，registry.h 也随之可见
# 预期: 编译通过
Step 3: task.c — 代价计算 switch→vtable
3.1 给每个 Upper Op 写 cost 包装函数
在 task.c 文件顶部（pg_cascades_check_limits 之后）新增 5 个包装函数：


/* ── 代价包装函数（从 ENFORCE_COMPUTE_COST switch 提取）── */

static void
pg_cost_agg(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
            PgGroupExpr *expr, double input_rows, int input_width,
            Cost child_startup, Cost child_total,
            Cost *out_startup, Cost *out_total)
{
    Path dummy_path;
    int  agg_strategy;

    agg_strategy = (expr->op == PG_CASCADES_PHYSICAL_HASHAGG)
                   ? AGG_HASHED : AGG_SORTED;

    MemSet(&dummy_path, 0, sizeof(Path));
    dummy_path.pathtype = T_Agg;
    cost_agg(&dummy_path, ctx->root, agg_strategy,
             &ctx->upper->agg_costs,
             ctx->upper->numGroupCols,
             ctx->upper->dNumGroups,
             child_startup, child_total, input_rows);
    *out_startup = dummy_path.startup_cost;
    *out_total   = dummy_path.total_cost;
}

static void
pg_cost_sort(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
             PgGroupExpr *expr, double input_rows, int input_width,
             Cost child_startup, Cost child_total,
             Cost *out_startup, Cost *out_total)
{
    Path    dummy_path;
    double  limit_tuples;
    PgSortPrivate *sp = (PgSortPrivate *) expr->op_private;

    limit_tuples = (sp && sp->limit_tuples > 0) ? sp->limit_tuples : -1.0;

    MemSet(&dummy_path, 0, sizeof(Path));
    dummy_path.pathtype = T_Sort;
    cost_sort(&dummy_path, ctx->root,
              (sp ? sp->pathkeys : NIL),
              child_total, input_rows, input_width,
              0.0, work_mem, limit_tuples);
    *out_startup = dummy_path.startup_cost;
    *out_total   = dummy_path.total_cost;
}

static void
pg_cost_unique(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
               PgGroupExpr *expr, double input_rows, int input_width,
               Cost child_startup, Cost child_total,
               Cost *out_startup, Cost *out_total)
{
    Path dummy_path;
    MemSet(&dummy_path, 0, sizeof(Path));
    dummy_path.pathtype = T_Unique;
    cost_sort(&dummy_path, ctx->root,
              ctx->upper->distinct_pathkeys,
              child_total, input_rows, input_width,
              0.0, work_mem, -1.0);
    *out_startup = dummy_path.startup_cost;
    *out_total   = dummy_path.total_cost;
}

static void
pg_cost_limit(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
              PgGroupExpr *expr, double input_rows, int input_width,
              Cost child_startup, Cost child_total,
              Cost *out_startup, Cost *out_total)
{
    double frac = 1.0;
    PgRequiredProperty *req = NULL;  /* 从 task->required 获取 */

    if (input_rows > 0)
        frac = Min(1.0, 1.0 / input_rows);  /* 默认 LIMIT 1 的分数 */

    *out_startup = child_startup;
    *out_total   = child_startup + (child_total - child_startup) * frac;
}

static void
pg_cost_project(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
                PgGroupExpr *expr, double input_rows, int input_width,
                Cost child_startup, Cost child_total,
                Cost *out_startup, Cost *out_total)
{
    QualCost qcost;
    MemSet(&qcost, 0, sizeof(QualCost));
    *out_startup = child_startup + qcost.startup;
    *out_total   = child_total + qcost.per_tuple * input_rows;
}
3.2 替换 ENFORCE_COMPUTE_COST 的 switch 体
定位到 case ENFORCE_COMPUTE_COST:（当前约 L1069），将 switch (expr->op) { ... } 整个替换为：


        case ENFORCE_COMPUTE_COST:
        {
            Cost child_startup = task->startup_cost;
            Cost child_total   = task->total_cost;
            double input_rows = 0;
            int    input_width = 0;
            PgGroupBestEntry *old_best = NULL;
            PgOperatorVtable *vt;

            /* ── 获取输入 rows/width（不变）── */
            if (list_length(expr->inputs) >= 1)
            {
                PgMemoGroup *child0 = (PgMemoGroup *)
                    linitial(expr->inputs);
                if (list_length(child0->best_entries) > 0)
                {
                    PgGroupBestEntry *cbe = (PgGroupBestEntry *)
                        linitial(child0->best_entries);
                    input_rows = cbe->output.rows;
                    input_width = cbe->output.width;
                }
            }
            if (input_rows <= 0) input_rows = 100;
            if (input_width <= 0) input_width = 10;

            /* ── 保存旧 best 用于 trace ── */
            {
                ListCell *blc;
                foreach(blc, expr->owner_group->best_entries)
                {
                    PgGroupBestEntry *be = (PgGroupBestEntry *) lfirst(blc);
                    if (pg_required_property_equal(be->required, task->required))
                    { old_best = be; break; }
                }
            }

            /* ── vtable 分发（替代 100 行 switch）── */
            vt = pg_registry_get_vtable(expr->op);
            if (vt && vt->cost_fn)
            {
                vt->cost_fn(ctx, expr->owner_group, expr,
                            input_rows, input_width,
                            child_startup, child_total,
                            &task->startup_cost, &task->total_cost);
            }
            else
            {
                /* 默认代价：微小固定成本（原 switch 的 default 分支） */
                task->startup_cost = child_startup + 0.01;
                task->total_cost   = child_total   + 0.01;
            }

            (void) 0;

            /* ── 后续逻辑不变（剪枝 + 属性检查 + best 更新）── */
            /* ... 从原来的 (void) 0; 之后保持不变 ... */
3.3 验证

make -C src/backend/optimizer/cascades -j$(nproc) && make -C src/backend -j$(nproc)
# 预期: 5 个 wrapper 函数在 task.c 编译通过，ENFORCE_COMPUTE_COST 中的 switch 已替换
# 运行时: vtable 均为 NULL（Step 6 才注册），走默认代价分支
Step 4: planbuild.c — Plan 提取 switch→vtable
4.1 给每个 Upper Op 写 build 包装函数
在 planbuild.c 中，将 SORT / HASHAGG / GROUPAGG / LIMIT / UNIQUE / PROJECT 的 case 体提取为 6 个独立函数：


/* ── Plan build 包装函数 ── */

static Plan *
pg_build_sort(PgPlannerCascadesContext *ctx, PgMemoGroup *group,
              PgGroupExpr *expr, PgGroupBestEntry *best,
              PgRequiredProperty *required, PgOutputProperty *output)
{
    /* 原 case PG_CASCADES_PHYSICAL_SORT: { ... } 的完整内容 */
    Plan *child;
    PgGroupBestEntry *child_best = NULL;
    PgOutputProperty child_out;
    PgMemoGroup *child_group;
    ListCell *lc;

    if (expr->inputs == NIL) return NULL;
    child_group = (PgMemoGroup *) linitial(expr->inputs);
    if (child_group->best_entries == NIL) return NULL;

    foreach(lc, child_group->best_entries)
    {
        PgGroupBestEntry *e = (PgGroupBestEntry *) lfirst(lc);
        if (best->child_required_props != NIL &&
            pg_required_property_equal(e->required,
                (PgRequiredProperty *) pg_safe_linitial_child_req(best)))
        { child_best = e; break; }
    }
    if (child_best == NULL) return NULL;

    child = pg_cascades_build_plan_recurse(ctx, child_group,
        (PgRequiredProperty *) pg_safe_linitial_child_req(best),
        child_best, &child_out);
    if (child == NULL) return NULL;

    *output = best->output;
    return (Plan *) make_sort_from_pathkeys(ctx->root, child,
        required->pathkeys, required->limit_tuples);
}

/* pg_build_hashagg, pg_build_groupagg, pg_build_limit, pg_build_unique, pg_build_project */
/* 同上模式，每个 20-40 行，从现有 case 体直接搬 */
4.2 替换 pg_cascades_build_plan_recurse 的 switch

/* ── 在 switch (expr->op) 之前添加 vtable 分发 ── */
{
    PgOperatorVtable *vt = pg_registry_get_vtable(expr->op);
    if (vt && vt->build_plan_fn)
    {
        Plan *result = vt->build_plan_fn(ctx, group, expr, best, required);
        if (result)
        {
            *output = best->output;
            return result;
        }
        return NULL;
    }
}

/* 后续 switch 保留 IMPORTED_PATH 分支 (SEQSCAN/INDEXSCAN/BITMAP/NESTLOOP/HASHJOIN/MERGEJOIN)
 * Upper Op 分支可以删除（已移到 wrapper 中注册） */
switch (expr->op)
{
    /* 保留: 这些走 IMPORTED_PATH */
    case PG_CASCADES_PHYSICAL_SEQSCAN:
    case PG_CASCADES_PHYSICAL_INDEXSCAN:
    case PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN:
    case PG_CASCADES_PHYSICAL_NESTLOOP:
    case PG_CASCADES_PHYSICAL_HASHJOIN:
    case PG_CASCADES_PHYSICAL_MERGEJOIN:
        // ... 现有 IMPORTED_PATH + COMPOSABLE_OP delegation ...
        break;
    /* Upper Op case 可以全部删除（注册时用 wrapper） */
    default:
        break;
}
4.3 验证

make -C src/backend/optimizer/cascades -j$(nproc)
# 预期: 6 个 wrapper 函数编译通过
Step 5: memo.c — Stats 推导 switch→vtable
5.1 给每个 Logical Op 写 stats wrapper

/* ── Stats wrapper 函数 ── */

static void
pg_derive_scan_stats(PgPlannerCascadesContext *ctx, PgGroupExpr *expr,
                     double *out_rows, int *out_width)
{
    /* 原 case PG_CASCADES_LOGICAL_SCAN 的内容 */
    if (expr->owner_group && expr->owner_group->rel) {
        *out_rows = expr->owner_group->rel->rows;
        *out_width = expr->owner_group->rel->width;
    } else if (expr->op_private) {
        RelOptInfo *rel = (RelOptInfo *) expr->op_private;
        *out_rows = rel->rows;
        *out_width = rel->width;
    }
}

static void
pg_derive_join_stats(PgPlannerCascadesContext *ctx, PgGroupExpr *expr,
                     double *out_rows, int *out_width)
{
    /* 原 case PG_CASCADES_LOGICAL_JOIN 的内容（~30行，搬过来） */
}

/* pg_derive_filter_stats, pg_derive_agg_stats, pg_derive_project_stats, 
   pg_derive_sort_stats, pg_derive_limit_stats, pg_derive_distinct_stats */
5.2 替换 pg_derive_expr_stats 的 switch

/* 在 switch 前加 vtable 查询 */
PgOperatorVtable *vt = pg_registry_get_vtable(expr->op);
if (vt && vt->derive_stats_fn) {
    vt->derive_stats_fn(ctx, expr, out_rows, out_width);
    return;
}

/* switch 保留作为 fallback（后续可逐步删除） */
switch (expr->op) { ... }
5.3 验证

make -C src/backend/optimizer/cascades -j$(nproc)
Step 6: 模块拆分 + 注册
6.1 模块文件列表（8 个新文件）

src/backend/optimizer/cascades/modules/
  scan.c      (~120行)
  filter.c    (~80行)
  project.c   (~90行)
  join.c      (~350行)
  agg.c       (~180行)
  sort.c      (~70行)
  limit.c     (~100行)
  distinct.c  (~40行)
6.2 模块文件模板（以 modules/join.c 为例）

/*-------------------------------------------------------------------------
 * modules/join.c
 *    JOIN 算子模块: NestLoop, HashJoin, MergeJoin + 所有 join 规则
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "optimizer/cascades.h"
#include "core/registry.h"

/* ── 前向声明: 现有 transform 函数 ── */
extern List *pg_rule_join_to_nestloop(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_join_to_hashjoin(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_join_to_mergejoin(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_join_to_nestloop_phase4(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_join_to_hashjoin_phase4(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_join_to_mergejoin_phase4(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_join_commutativity(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_join_associativity(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_join_left_asscom(PgPlannerCascadesContext *, PgGroupExpr *);
extern List *pg_rule_inner_to_semi(PgPlannerCascadesContext *, PgGroupExpr *);
/* ... 其余 join 相关规则 ... */

/* ── Pattern 引用 ── */
extern Pattern *g_pat_join_leaf_leaf;
extern Pattern *g_pat_join_join_leaf_leaf_leaf;
extern Pattern *g_pat_join_leaf_join_leaf_leaf;

/* ── 模块 init ── */
void
pg_module_join_init(void)
{
    /* 注册物理算子 vtable（cost/build 用 NULL → IMPORTED_PATH 委托） */
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_NESTLOOP,
        .name = "NestLoop",
        /* cost_fn = NULL    → 走默认代价 0.01 */
        /* build_plan_fn = NULL → 走 IMPORTED_PATH 委托 */
        /* derive_stats_fn = NULL → 走默认推导 */
    });
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_HASHJOIN,  .name = "HashJoin",
    });
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_MERGEJOIN, .name = "MergeJoin",
    });

    /* ── 实现规则 ── */
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalJoin→PhysicalNestLoop",
        PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_NESTLOOP,
        g_pat_join_leaf_leaf, pg_rule_join_to_nestloop,
        0.5, PG_RULE_BIT_JOIN_TO_NESTLOOP);

    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalJoin→PhysicalHashJoin",
        PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_HASHJOIN,
        g_pat_join_leaf_leaf, pg_rule_join_to_hashjoin,
        0.5, PG_RULE_BIT_JOIN_TO_HASHJOIN);

    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalJoin→PhysicalMergeJoin",
        PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_MERGEJOIN,
        g_pat_join_leaf_leaf, pg_rule_join_to_mergejoin,
        0.5, PG_RULE_BIT_JOIN_TO_MERGEJOIN);

    /* Phase 4 join 规则（promise 0.9，优先于 COMPOSABLE_OP） */
    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalJoin→PhysicalHashJoin_Phase4",
        PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_HASHJOIN,
        NULL, pg_rule_join_to_hashjoin_phase4,
        0.9, PG_RULE_BIT_JOIN_TO_HASHJOIN_PHASE4);

    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalJoin→PhysicalNestLoop_Phase4",
        PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_NESTLOOP,
        NULL, pg_rule_join_to_nestloop_phase4,
        0.9, PG_RULE_BIT_JOIN_TO_NESTLOOP_PHASE4);

    pg_registry_register_rule(PG_RULE_MEMO_IMPL,
        "LogicalJoin→PhysicalMergeJoin_Phase4",
        PG_CASCADES_LOGICAL_JOIN, PG_CASCADES_PHYSICAL_MERGEJOIN,
        NULL, pg_rule_join_to_mergejoin_phase4,
        0.9, PG_RULE_BIT_JOIN_TO_MERGEJOIN_PHASE4);

    /* ── 变换规则 ── */
    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "JoinCommutativity",
        PG_CASCADES_LOGICAL_JOIN, 0,
        g_pat_join_leaf_leaf, pg_rule_join_commutativity,
        0.5, PG_RULE_BIT_JOIN_COMMUTATIVITY);

    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "JoinAssociativity",
        PG_CASCADES_LOGICAL_JOIN, 0,
        g_pat_join_join_leaf_leaf_leaf, pg_rule_join_associativity,
        0.2, PG_RULE_BIT_JOIN_ASSOCIATIVITY);

    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "JoinLeftAsscom",
        PG_CASCADES_LOGICAL_JOIN, 0,
        g_pat_join_leaf_join_leaf_leaf, pg_rule_join_left_asscom,
        0.2, PG_RULE_BIT_JOIN_LEFT_ASSCOM);

    pg_registry_register_rule(PG_RULE_MEMO_TRANSFORM,
        "InnerJoinToSemi",
        PG_CASCADES_LOGICAL_JOIN, 0,
        g_pat_join_leaf_leaf, pg_rule_inner_to_semi,
        0.3, PG_RULE_BIT_INNER_TO_SEMI);

    /* ... OuterJoinElimination, EliminateJoinWithConst, PruneEmptyJoin,
     *     PushDownPredicateJoin, PruneJoinColumns, MergeFilterWithJoin,
     *     MergeJoinWithChildProj ...
     *   每条规则一个 pg_registry_register_rule 调用 */
}
6.3 Makefile 更新

OBJS = cascades.o memo.o task.o property.o pattern.o \
       pg_adapter.o planbuild.o debug.o postopt.o rewrite.o \
       core/registry.o \
       modules/scan.o modules/filter.o modules/project.o \
       modules/join.o modules/agg.o modules/sort.o \
       modules/limit.o modules/distinct.o
6.4 cascades.c init

/* 在 pg_cascades_ensure_hook 调用之前，pg_cascades_init 中调用 */

void pg_cascades_init(void)
{
    pg_registry_init();

    /* 核心算子模块（按依赖顺序） */
    pg_module_scan_init();
    pg_module_filter_init();
    pg_module_project_init();
    pg_module_join_init();
    pg_module_agg_init();
    pg_module_sort_init();
    pg_module_limit_init();
    pg_module_distinct_init();
}
6.5 cascades.c 规则获取简化

/* 前 */
rules = pg_cascades_get_impl_rules(&num_impl);
// ... 5 组合并, 按 promise 排序 ...
ctx.impl_rules = merged;
ctx.trans_rules = merged2;

/* 后 */
ctx.impl_rules = pg_registry_get_rules(PG_RULE_MEMO_IMPL);
ctx.trans_rules = pg_registry_get_rules(PG_RULE_MEMO_TRANSFORM);
ctx.num_impl_rules = list_length(ctx.impl_rules);
ctx.num_trans_rules = list_length(ctx.trans_rules);
6.6 删除 rule.c
rule.c 的 2700 行全部拆分到 8 个模块文件后，rule.c 整体删除。Makefile 中去掉 rule.o。

6.7 验证

make -C src/backend/optimizer/cascades -j$(nproc)
make -C src/backend -j$(nproc)
make install
pg_ctl restart
psql -d cascades_test -f test/cascades_coverage_extension.sql
# 预期: 5072 全部通过，0 回归
各步骤依赖关系

Step 1 (registry.c)         ← 无依赖，最先做
  ↓
Step 2 (cascades.h include) ← 依赖 Step 1
  ↓
Step 3 (task.c) ←──┐
Step 4 (planbuild.c) ├── 可并行
Step 5 (memo.c) ←──┘
  ↓
Step 6 (module split)       ← 依赖 Step 1-5 全部完成
代码量
Step	+行	-行
1. registry.h + registry.c	220	0
2. cascades.h	2	0
3. task.c	90	100
4. planbuild.c	150	200
5. memo.c	100	30
6. 8 modules + 删 rule.c	1030	2700
合计	1592	3030
净减		1438
