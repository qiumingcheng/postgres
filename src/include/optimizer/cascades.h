/*-------------------------------------------------------------------------
 *
 * cascades.h
 *    Cascades 优化器核心头文件 —— 所有数据结构定义
 *
 * 基于 StarRocks Cascades 优化器设计，迁移到 PostgreSQL 9.2.4。
 *
 *-------------------------------------------------------------------------
 */

#ifndef CASCADES_H
#define CASCADES_H

#include "postgres.h"
#include "nodes/relation.h"
#include "nodes/primnodes.h"
#include "nodes/plannodes.h"
#include "utils/timestamp.h"
#include "utils/hsearch.h"

/* ========================================================================
 * 枚举定义
 * ======================================================================== */

/* 逻辑和物理算子种类 */
typedef enum PgCascadesOpKind
{
    /* Logical operators */
    PG_CASCADES_LOGICAL_SCAN,
    PG_CASCADES_LOGICAL_FILTER,
    PG_CASCADES_LOGICAL_PROJECT,
    PG_CASCADES_LOGICAL_JOIN,
    PG_CASCADES_LOGICAL_AGG,
    PG_CASCADES_LOGICAL_DISTINCT,
    PG_CASCADES_LOGICAL_SORT,
    PG_CASCADES_LOGICAL_LIMIT,

    /* Physical operators */
    PG_CASCADES_PHYSICAL_SEQSCAN,
    PG_CASCADES_PHYSICAL_INDEXSCAN,
    PG_CASCADES_PHYSICAL_BITMAP_HEAPSCAN,
    PG_CASCADES_PHYSICAL_BITMAP_AND,
    PG_CASCADES_PHYSICAL_BITMAP_OR,
    PG_CASCADES_PHYSICAL_NESTLOOP,
    PG_CASCADES_PHYSICAL_HASHJOIN,
    PG_CASCADES_PHYSICAL_MERGEJOIN,
    PG_CASCADES_PHYSICAL_HASHAGG,
    PG_CASCADES_PHYSICAL_GROUPAGG,
    PG_CASCADES_PHYSICAL_SORT,
    PG_CASCADES_PHYSICAL_UNIQUE,
    PG_CASCADES_PHYSICAL_LIMIT,
    PG_CASCADES_PHYSICAL_PROJECT
} PgCascadesOpKind;

/* Physical expression 模式：导入的完整 Path vs 可组合算子 */
typedef enum PgPhysicalExprMode
{
    PG_PHYS_EXPR_IMPORTED_PATH,     /* op_private 是完整 Path * */
    PG_PHYS_EXPR_COMPOSABLE_OP      /* op_private 是算子参数，child group 待选择 */
} PgPhysicalExprMode;

/* Cascades 执行状态 */
typedef enum PgCascadesStatus
{
    PG_CASCADES_OK = 0,

    /* 语义不支持（直接 fallback） */
    PG_CASCADES_UNSUPPORTED,
    PG_CASCADES_UNSUPPORTED_RTE_KIND,
    PG_CASCADES_UNSUPPORTED_INHERITANCE,
    PG_CASCADES_UNSUPPORTED_FDW,
    PG_CASCADES_UNSUPPORTED_RELKIND,
    PG_CASCADES_UNSUPPORTED_SUBPLAN,
    PG_CASCADES_UNSUPPORTED_WINDOW,
    PG_CASCADES_UNSUPPORTED_SETOP,

    /* 内部失败（按 GUC 决定 fallback 或 ERROR） */
    PG_CASCADES_INTERNAL_LIMIT,
    PG_CASCADES_INTERNAL_TIMEOUT,
    PG_CASCADES_INTERNAL_NO_PLAN
} PgCascadesStatus;

/* Task 类型 */
typedef enum PgTaskType
{
    PG_TASK_OPTIMIZE_GROUP,
    PG_TASK_OPTIMIZE_EXPRESSION,
    PG_TASK_EXPLORE_GROUP,
    PG_TASK_DERIVE_STATS,
    PG_TASK_APPLY_RULE,
    PG_TASK_ENFORCE_AND_COST
} PgTaskType;

/* ========================================================================
 * 前向声明
 * ======================================================================== */

typedef struct PgMemo               PgMemo;
typedef struct PgMemoGroup          PgMemoGroup;
typedef struct PgGroupExpr          PgGroupExpr;
typedef struct PgRequiredProperty   PgRequiredProperty;
typedef struct PgOutputProperty     PgOutputProperty;
typedef struct PgGroupBestEntry     PgGroupBestEntry;
typedef struct PgOptimizerTask      PgOptimizerTask;
typedef struct PgRule               PgRule;
typedef struct PgPlannerCascadesContext PgPlannerCascadesContext;
typedef struct PgCascadesUpperInfo  PgCascadesUpperInfo;
typedef struct QueryPlannerPrepResult QueryPlannerPrepResult;

/* ========================================================================
 * 核心数据结构
 * ======================================================================== */

/* Required Property: 父节点要求子节点满足的物理属性 */
struct PgRequiredProperty
{
    List       *pathkeys;           /* canonical PathKey list，NIL = 无排序要求 */
    Relids      required_outer;     /* 参数化路径依赖 */
    double      tuple_fraction;     /* row goal: LIMIT / cursor / EXISTS */
    double      limit_tuples;       /* LIMIT 行数 */
};

/* Output Property: 物理表达式实际输出的属性 */
struct PgOutputProperty
{
    List       *pathkeys;           /* canonical PathKey list */
    Relids      required_outer;     /* 输出的参数化依赖 */
    double      rows;               /* 估算输出行数 */
    int         width;              /* 估算输出宽度 */
};

/* GroupExpression: Memo 中的一种计算方式 */
struct PgGroupExpr
{
    PgCascadesOpKind op;            /* 算子类型 */
    PgPhysicalExprMode mode;        /* IMPORTED_PATH or COMPOSABLE_OP */
    List       *inputs;             /* List<PgMemoGroup *>，子 Group */
    Bitmapset  *applied_rules;      /* 已应用规则位图 */
    bool        stats_derived;      /* 统计信息是否已推导 */

    void       *op_private;         /* Path*, RTE, RestrictInfo list, Agg info 等 */

    PgMemoGroup *owner_group;       /* 所属 Group */
};

/* GroupBestEntry: 特定 required property 下的最优表达式 */
struct PgGroupBestEntry
{
    PgRequiredProperty *required;   /* hash key */
    PgGroupExpr  *expr;             /* best expression */
    Cost          startup_cost;
    Cost          total_cost;
    List         *child_required_props; /* List<PgRequiredProperty *> */
    PgOutputProperty output;        /* 该 expression 在该 required 下的输出 */
};

/* MemoGroup: 等价结果集合 */
struct PgMemoGroup
{
    int         id;
    List       *logical_exprs;      /* List<PgGroupExpr *> */
    List       *physical_exprs;     /* List<PgGroupExpr *> */

    double      rows;               /* 估算行数 */
    int         width;              /* 估算宽度 */

    List       *best_entries;       /* List<PgGroupBestEntry *> */
    RelOptInfo *rel;               /* 仅当此 group 映射到一个 PG 关系时 */
};

/* Memo: Cascades 搜索空间 */
struct PgMemo
{
    MemoryContext context;
    List       *groups;             /* List<PgMemoGroup *> */
    HTAB       *group_expr_table;   /* 去重 hash 表（第一版可为 NULL） */
    PgMemoGroup *root_group;
};

/* OptimizerTask: task scheduler 栈中的任务 */
struct PgOptimizerTask
{
    PgTaskType   type;
    PgMemoGroup *group;             /* for OptimizeGroup / ExploreGroup */
    PgGroupExpr *expr;              /* for OptimizeExpression / ApplyRule / EnforceAndCost */
    PgRequiredProperty *required;   /* for EnforceAndCost */
    void       *rule;               /* for ApplyRule, PgRule * */

    bool        is_resume;          /* true: 因 child 未就绪而暂停 */
    int         resume_child_idx;
    List       *child_best_results;
};

/* Rule: 一条变换规则 */
typedef bool (*PgRuleMatchFn)(PgGroupExpr *expr);
typedef List *(*PgRuleTransformFn)(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);

struct PgRule
{
    const char *name;
    PgRuleMatchFn     match;        /* 匹配函数，第一版可为 NULL */
    PgRuleTransformFn transform;    /* 变换函数 */
    bool        is_implementation;  /* true=implementation, false=transformation */
    PgCascadesOpKind from_op;       /* 匹配的 logical op kind */
    PgCascadesOpKind to_op;        /* 目标 op kind */
};

/* ========================================================================
 * 上下文结构
 * ======================================================================== */

/* Upper Info: grouping_planner 传给 Cascades 的上层语义 */
struct PgCascadesUpperInfo
{
    List       *tlist;
    List       *sub_tlist;
    AttrNumber *groupColIdx;
    bool        need_tlist_eval;

    AggClauseCosts agg_costs;
    int         numGroupCols;
    double      dNumGroups;

    double      tuple_fraction;
    double      limit_tuples;
    double      sub_limit_tuples;
    int64       offset_est;
    int64       count_est;

    List       *activeWindows;      /* 第一版要求 NIL */

    bool        hasAggs;
    List       *groupClause;
    List       *distinctClause;
    List       *sortClause;
    Node       *havingQual;
    bool        hasDistinctOn;

    List       *group_pathkeys;
    List       *sort_pathkeys;
    List       *distinct_pathkeys;
    Oid        *groupOperators;
};

/* QueryPlannerPrepResult: prepare 阶段的输出 */
struct QueryPlannerPrepResult
{
    bool        trivial_result;
    Path       *trivial_path;
    List       *joinlist;
    double      total_table_pages;

    bool        lower_paths_built;
    RelOptInfo *final_rel;
};

/* PlannerCascadesContext: Cascades 搜索的总上下文 */
struct PgPlannerCascadesContext
{
    PlannerInfo           *root;
    PgMemo                *memo;
    PgCascadesUpperInfo   *upper;
    QueryPlannerPrepResult *prep;

    /* Task stack — LIFO */
    List        *task_stack;

    /* Rule set */
    PgRule     *impl_rules;
    int          num_impl_rules;
    PgRule     *trans_rules;
    int          num_trans_rules;

    /* 限制配置 */
    int          max_groups;
    int          max_tasks;
    int          timeout_ms;
    TimestampTz  start_time;
    int          num_tasks_executed;

    /* 调试 */
    bool         debug;
    List        *fallback_reasons;

    /* 内存 */
    MemoryContext memo_cxt;
    MemoryContext task_cxt;
};

/* ========================================================================
 * GUC 变量声明
 * ======================================================================== */

extern bool enable_cascades_planner;
extern bool cascades_planner_debug;
extern bool cascades_planner_fallback_on_error;
extern int  cascades_planner_timeout_ms;
extern int  cascades_planner_max_groups;
extern int  cascades_planner_max_tasks;

/* ========================================================================
 * 核心函数声明
 * ======================================================================== */

/* cascades.c */
extern PgCascadesStatus pg_cascades_try_grouping_planner(
    PlannerInfo *root, QueryPlannerPrepResult *prep,
    PgCascadesUpperInfo *upper, Plan **plan);
extern PgCascadesStatus pg_cascades_supported_query_precheck(
    PlannerInfo *root, PgCascadesUpperInfo *upper);
extern PgCascadesStatus pg_cascades_supported_query(
    PlannerInfo *root, PgCascadesUpperInfo *upper);
extern void pg_cascades_handle_status_or_error(PgCascadesStatus status,
                                                bool debug);

/* memo.c */
extern PgMemo *pg_memo_init(PgPlannerCascadesContext *ctx,
                             PgGroupExpr *logical_root);
extern PgMemoGroup *pg_memo_new_group(PgPlannerCascadesContext *ctx);
extern PgGroupExpr *pg_memo_new_group_expr(PgPlannerCascadesContext *ctx,
                                            PgCascadesOpKind op);
extern void pg_memo_add_physical_expr(PgMemoGroup *group, PgGroupExpr *expr);
extern void pg_memo_add_logical_expr(PgMemoGroup *group, PgGroupExpr *expr);
extern PgMemoGroup *pg_memo_insert_expression(PgPlannerCascadesContext *ctx,
    PgMemo *memo, PgGroupExpr *expr, PgMemoGroup *parent_group);
extern void pg_memo_derive_logical_property(PgMemo *memo, PgMemoGroup *group,
                                             PgPlannerCascadesContext *ctx);

/* task.c */
extern bool task_stack_empty(PgPlannerCascadesContext *ctx);
extern void task_stack_push(PgPlannerCascadesContext *ctx, PgOptimizerTask *task);
extern PgOptimizerTask *task_stack_pop(PgPlannerCascadesContext *ctx);
extern PgCascadesStatus pg_cascades_check_limits(PgPlannerCascadesContext *ctx);
extern PgCascadesStatus pg_cascades_run_tasks(PgPlannerCascadesContext *ctx);
extern void pg_cascades_push_enforce_and_cost_tasks(
    PgPlannerCascadesContext *ctx, PgMemoGroup *group, PgGroupExpr *expr);

/* property.c */
extern bool pg_required_property_equal(const PgRequiredProperty *a,
                                        const PgRequiredProperty *b);
extern PgRequiredProperty *pg_required_property_copy(
    PgPlannerCascadesContext *ctx, const PgRequiredProperty *p);
extern bool pg_output_satisfies_required(const PgOutputProperty *out,
                                          const PgRequiredProperty *req);
extern PgRequiredProperty *pg_cascades_root_required_property(
    PgPlannerCascadesContext *ctx);
extern void pg_derive_child_properties(PgPlannerCascadesContext *ctx,
    PgGroupExpr *expr, PgRequiredProperty *required,
    List **child_required_props, PgOutputProperty *output);
extern void pg_group_update_best(PgMemoGroup *group,
                                  PgGroupBestEntry *new_entry);

/* rule.c */
extern PgRule *pg_cascades_get_impl_rules(int *num_rules);
extern PgRule *pg_cascades_get_trans_rules(int *num_rules);
extern PgRule *pg_cascades_get_impl_rules_phase2(int *num_rules);

/* pg_adapter.c */
extern PgGroupExpr *pg_cascades_build_logical_root(
    PgPlannerCascadesContext *ctx);
extern PgCascadesOpKind pg_cascades_pathtype_to_opkind(NodeTag pathtype);

/* planbuild.c */
extern Plan *pg_cascades_extract_best_plan(PgPlannerCascadesContext *ctx);

/* debug.c */
extern void debug_print_cascades_memo(PgPlannerCascadesContext *ctx);
extern void debug_print_cascades_fallback_reason(
    PgPlannerCascadesContext *ctx, const char *reason);

#endif /* CASCADES_H */
