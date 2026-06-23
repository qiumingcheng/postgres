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
    PG_CASCADES_LOGICAL_UNION,      /* Phase 5: UNION ALL / UNION */

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

/*
 * Phase 4: Per-rule bit indices for BitSet tracking.
 *    explored_rules: which rules have been tried on this expression
 *    applied_rules:  which rules produced this expression (lineage)
 */
#define PG_RULE_BIT_AGG_TO_HASHAGG         0
#define PG_RULE_BIT_AGG_TO_GROUPAGG        1
#define PG_RULE_BIT_SORT_TO_SORT           2
#define PG_RULE_BIT_DISTINCT_TO_UNIQUE     3
#define PG_RULE_BIT_LIMIT_TO_LIMIT         4
#define PG_RULE_BIT_PROJECT_TO_PROJECT     5
#define PG_RULE_BIT_SCAN_TO_SEQSCAN        6
#define PG_RULE_BIT_SCAN_TO_INDEXSCAN      7
#define PG_RULE_BIT_SCAN_TO_BITMAPSCAN     8
#define PG_RULE_BIT_JOIN_TO_NESTLOOP       9
#define PG_RULE_BIT_JOIN_TO_HASHJOIN      10
#define PG_RULE_BIT_JOIN_TO_MERGEJOIN     11
#define PG_RULE_BIT_JOIN_COMMUTATIVITY    12

/* Phase 5: Transformation rules (27 rules) */
#define PG_RULE_BIT_MERGE_PROJECT          13  /* H2 */
#define PG_RULE_BIT_PRUNE_EMPTY_SCAN       14  /* E2 */
#define PG_RULE_BIT_ELIMINATE_PROJECT      15  /* H3 */
#define PG_RULE_BIT_PUSHDOWN_PRED_SCAN     16  /* A1 */
#define PG_RULE_BIT_MERGE_LIMIT_SORT       17  /* D1 */
#define PG_RULE_BIT_ELIM_SORT_CONST_KEY    18  /* H1 */
#define PG_RULE_BIT_MERGE_FILTER_JOIN      19  /* H4 */
#define PG_RULE_BIT_PRUNE_EMPTY_UNION      20  /* E3 */
#define PG_RULE_BIT_MERGE_TWO_AGG          21  /* F2 */
#define PG_RULE_BIT_MERGE_JOIN_PROJ        22  /* G4 */
#define PG_RULE_BIT_PRUNE_AGG_COLS         23  /* B3 */
#define PG_RULE_BIT_PRUNE_PROJ_COLS        24  /* B4 */
#define PG_RULE_BIT_PRUNE_SORT_COLS        25  /* B5 */
#define PG_RULE_BIT_PRUNE_EMPTY_JOIN       26  /* E1 */
#define PG_RULE_BIT_PRUNE_SCAN_COLS        27  /* B1 */
#define PG_RULE_BIT_PRUNE_JOIN_COLS        28  /* B2 */
#define PG_RULE_BIT_PUSHDOWN_PRED_PROJ     29  /* A4 */
#define PG_RULE_BIT_PUSHDOWN_LIMIT_JOIN    30  /* D2 */
#define PG_RULE_BIT_PUSHDOWN_PRED_AGG      31  /* A3 */
#define PG_RULE_BIT_PUSHDOWN_AGG_LIMIT     32  /* F3 */
#define PG_RULE_BIT_ELIM_JOIN_CONST        33  /* G1 */
#define PG_RULE_BIT_OUTER_JOIN_ELIM        34  /* G2 */
#define PG_RULE_BIT_PUSHDOWN_PRED_JOIN     35  /* A2 */
#define PG_RULE_BIT_INNER_TO_SEMI          36  /* G3 */
#define PG_RULE_BIT_JOIN_ASSOCIATIVITY     37  /* C2 */
#define PG_RULE_BIT_JOIN_LEFT_ASSCOM       38  /* C3 */
#define PG_RULE_BIT_ELIMINATE_LIMIT        39  /* D3 */
#define PG_RULE_BIT_ELIMINATE_AGG          40  /* F1 */
#define PG_RULE_BIT_PUSHDOWN_PRED_UNION    41  /* A5 */

/* Enforcer rule */
#define PG_RULE_BIT_ENFORCE_SORT           42  /* Sort enforcer */

/* Phase 4: Path-generation join rules (call make_join_rel internally) */
#define PG_RULE_BIT_JOIN_TO_HASHJOIN_PHASE4  43  /* Phase4 HashJoin */
#define PG_RULE_BIT_JOIN_TO_NESTLOOP_PHASE4  44  /* Phase4 NestLoop */
#define PG_RULE_BIT_JOIN_TO_MERGEJOIN_PHASE4 45  /* Phase4 MergeJoin */

/* Phase 6: additional transformation rules */
#define PG_RULE_BIT_MERGE_LIMIT_CHILD_LIMIT 46  /* H5 */

#define PG_RULE_BIT_MAX                   47

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
    Bitmapset  *required_columns;   /* Phase5: 上层需要的列 (Var varattno 集合) */
};

/* Output Property: 物理表达式实际输出的属性 */
struct PgOutputProperty
{
    List       *pathkeys;           /* canonical PathKey list */
    Relids      required_outer;     /* 输出的参数化依赖 */
    double      rows;               /* 估算输出行数 */
    int         width;              /* 估算输出宽度 */
};

/*
 * PgLogicalProperty: Phase 4 logical property per group.
 * Derived bottom-up before cost-based search.
 */
typedef struct PgLogicalProperty
{
    Relids      relids;             /* base rel OIDs in this group */
    double      rows;               /* estimated row count */
    int         width;              /* estimated row width */
    Bitmapset  *output_columns;     /* columns this group outputs */
    bool        has_subquery;       /* contains correlated subquery */
} PgLogicalProperty;

/* GroupExpression: Memo 中的一种计算方式 */
struct PgGroupExpr
{
    PgCascadesOpKind op;            /* 算子类型 */
    PgPhysicalExprMode mode;        /* IMPORTED_PATH or COMPOSABLE_OP */
    List       *inputs;             /* List<PgMemoGroup *>，子 Group */
    Bitmapset  *applied_rules;      /* 已应用规则位图 */
    Bitmapset  *explored_rules;     /* Phase 4: 已尝试探索规则 */
    uint32      expr_hash;           /* Phase 4: hash for dedup */
    bool        stats_derived;      /* 统计信息是否已推导 */
    Cost        best_cost;           /* Phase 6: cached best total_cost for pruning */

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
    PgLogicalProperty logical_prop; /* Phase 4: derived logical property */
    double      lower_bound_cost;   /* Phase 6: cost lower bound for pruning */
    bool        optimized;          /* Phase 6: has this group been optimized? */
};

/* Memo: Cascades 搜索空间 */
struct PgMemo
{
    MemoryContext context;
    List       *groups;             /* List<PgMemoGroup *> */
    HTAB       *group_expr_table;   /* 去重 hash 表（第一版可为 NULL） */
    PgMemoGroup *root_group;
};

/* Phase 4: Enforcer state machine states */
typedef enum PgEnforceState
{
    ENFORCE_INIT,              /* 初始化：确定 output property */
    ENFORCE_OPTIMIZE_CHILDREN, /* 优化子节点（逐个） */
    ENFORCE_COMPUTE_COST,      /* 所有子节点完成 → 计算总代价 */
    ENFORCE_ENFORCE_PROPERTY,  /* 属性不满足 → 应用 Enforcer */
    ENFORCE_COMPLETE            /* 完成 */
} PgEnforceState;

/* Forward declarations for types used in PgOptimizerTask and PgRule */
typedef struct PgPattern  PgPattern;
typedef struct PgBinder   PgBinder;

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

    /* Phase 4: Enforcer state machine */
    PgEnforceState enforce_state;   /* current state */
    int         cur_child_index;    /* current child being optimized */
    PgOutputProperty output_property; /* derived output */
    List       *child_required_props; /* per-child required properties */
    double      total_cost;         /* accumulated cost */
    double      startup_cost;       /* startup cost */

    /* Phase 5: Pattern binding result */
    PgBinder   *binder;             /* Pattern 绑定结果（仅 pattern-based 规则使用） */
};

/* Rule: 一条变换规则 */
typedef bool (*PgRuleMatchFn)(PgGroupExpr *expr);
typedef List *(*PgRuleTransformFn)(PgPlannerCascadesContext *ctx, PgGroupExpr *expr);

/* Phase 5: 规则类型 */
typedef enum PgRuleType
{
    PG_RULE_IMPL,            /* 实现规则: Logical → Physical */
    PG_RULE_TRANS,           /* 变换规则: Logical → Logical */
    PG_RULE_ENFORCER         /* Enforcer: 插入 Sort 等 */
} PgRuleType;

struct PgRule
{
    const char *name;
    PgRuleMatchFn     match;        /* 匹配函数，可为 NULL */
    PgRuleTransformFn transform;    /* 变换函数 */

    PgRuleType   rule_type;         /* Phase 5: PG_RULE_IMPL/TRANS/ENFORCER */

    /* 匹配方式：pattern 优先于 from_op */
    PgPattern       *pattern;       /* Phase 5: 多节点 Pattern */
    PgCascadesOpKind from_op;       /* 单节点匹配（pattern==NULL 时使用） */
    PgCascadesOpKind to_op;         /* 目标 op kind（仅 rule_type==IMPL 有效） */

    int         rule_bit;           /* Phase 4: rule index for BitSet */
    double      promise;            /* Phase 4: expected benefit 0..1 */
};

/* ========================================================================
 * Phase 5: Rule 私有数据结构
 * ======================================================================== */

/* PgJoinPrivate: LogicalJoin/PhysicalJoin 的 op_private */
typedef struct PgJoinPrivate
{
    JoinType    jointype;           /* JOIN_INNER, JOIN_LEFT, etc. */
    List       *restrictlist;       /* join qual RestrictInfo list */
    List       *joinlist;           /* deconstruct_jointree 的子 joinlist */
} PgJoinPrivate;

/* PgSortPrivate: LogicalSort/PhysicalSort 的 op_private */
typedef struct PgSortPrivate
{
    List       *pathkeys;           /* canonical PathKey list */
    double      limit_tuples;       /* 0 = 无 limit, >0 = TopN bound */
} PgSortPrivate;

/* ========================================================================
 * Phase 4: Pattern Matching Engine
 * ======================================================================== */

typedef enum PgPatternNodeType
{
    PG_PATTERN_LEAF,         /* 匹配任意 Group (通配) */
    PG_PATTERN_MULTI_LEAF,   /* 匹配多个 Group (用于 n-ary Join) */
    PG_PATTERN_OPERATOR,     /* 匹配特定算子类型 */
    PG_PATTERN_TREE           /* 匹配子树（递归） */
} PgPatternNodeType;

typedef struct PgPattern
{
    PgPatternNodeType type;
    PgCascadesOpKind  op;           /* 仅 PATTERN_OPERATOR 有效 */
    List             *children;     /* List<PgPattern *>，PATTERN_TREE 的子模式 */
} PgPattern;

typedef struct PgBinder
{
    PgPattern   *pattern;       /* 匹配的 pattern 节点 */
    PgGroupExpr *expr;          /* 匹配到的 expression */
    PgMemoGroup *group;         /* expression 所在的 group */
    List        *child_matches; /* List<PgBinder *>，子匹配 */
} PgBinder;

/* Phase 4: CombinationRule — 一组相关规则的聚合 */
typedef struct PgCombinationRule
{
    const char *name;
    int        *rule_ids;       /* array of rule_bit indices, terminated by -1 */
    bool        iterate;         /* iterate until convergence */
} PgCombinationRule;

/* Phase 4a: Rewrite Stage — 分阶段规则重写 pipeline */
typedef enum PgRewriteStage
{
    REWRITE_CTE_INLINE,
    REWRITE_SUBQUERY,
    REWRITE_PREDICATE_PUSHDOWN,
    REWRITE_COLUMN_PRUNE,
    REWRITE_JOIN_REORDER,
    REWRITE_LIMIT_PUSH,
    REWRITE_AGG_PUSHDOWN,
    REWRITE_SEMIJOIN_DEDUP,
    REWRITE_NUM_STAGES
} PgRewriteStage;

/* RewriteRule: a single named rule in a pipeline stage */
typedef struct PgRewriteRule
{
    const char     *name;          /* rule name (matches PgRule.name) */
    PgCascadesOpKind match_op;     /* which logical op to match */
    PgRuleTransformFn transform;   /* transform function */
    int             rule_bit;      /* for BitSet tracking */
    double          promise;       /* priority */
} PgRewriteRule;

typedef struct PgRewriteStageDef
{
    PgRewriteStage  stage;
    const char     *name;
    bool            iterate;       /* iterate until convergence */
    PgRewriteRule  *rules;         /* array of rules, sentinel-terminated */
    int             num_rules;     /* number of rules (excluding sentinel) */
} PgRewriteStageDef;

/* Pattern 构造函数 */
PgPattern *pg_pattern_leaf(void);
PgPattern *pg_pattern_multi_leaf(void);
PgPattern *pg_pattern_op(PgCascadesOpKind op);
PgPattern *pg_pattern_tree(PgCascadesOpKind op, List *children);

/* Pattern 匹配 */
List *pg_pattern_match_root_only(PgPattern *pattern, PgGroupExpr *root);
List *pg_pattern_match_full(PgPattern *pattern, PgGroupExpr *root);

/* ========================================================================
 * Hash Key for GroupExpression Dedup (shared by memo.c and pg_adapter.c)
 * ======================================================================== */

#define PG_MEMO_HASH_MAX_INPUTS 4

typedef struct PgExprHashKey
{
    PgCascadesOpKind op;
    int32           num_inputs;
    int32           group_ids[PG_MEMO_HASH_MAX_INPUTS];
} PgExprHashKey;

/*
 * PgExprHashEntry: hash table entry for global GroupExpression dedup.
 * The hash key (op + group_ids) is used for lookup; owner_group_id
 * records which group first created this expression.  When a duplicate
 * is later inserted into a different group, the two groups are merged.
 */
typedef struct PgExprHashEntry
{
    PgExprHashKey  key;              /* hash key (op + input group IDs) */
    int32          owner_group_id;    /* group that first inserted this expr */
} PgExprHashEntry;

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

    double       upper_bound_cost;       /* Phase 4: pruning bound */

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
extern PgMemoGroup *pg_memo_insert_expression_tree(PgPlannerCascadesContext *ctx,
    PgGroupExpr *tree_root);
extern void pg_memo_merge_group(PgPlannerCascadesContext *ctx,
                                 PgMemoGroup *target, PgMemoGroup *source);
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
extern PgRule *pg_cascades_get_impl_rules_phase2_scan(int *num_rules);
extern PgRule *pg_cascades_get_impl_rules_phase2_join(int *num_rules);
extern PgRule *pg_cascades_get_trans_rules_phase5(int *num_rules);
extern PgRule *pg_cascades_get_enforcer_rules(int *num_rules);
extern PgRule *pg_cascades_get_rules_sorted(PgRule *rules, int *num_rules);
extern void pg_cascades_init_rule_patterns(void);

/* Phase 5: Binder helper for pattern-based rules */
extern PgBinder *pg_cascades_get_current_binder(PgPlannerCascadesContext *ctx);

/* Phase 5: Group-to-RelOptInfo mapping */
extern RelOptInfo *pg_cascades_group_to_rel(PgPlannerCascadesContext *ctx,
                                             PgMemoGroup *group);
extern Relids pg_cascades_group_relids(PgPlannerCascadesContext *ctx,
                                        PgMemoGroup *group);

/* pg_adapter.c */
extern PgCascadesOpKind pg_cascades_pathtype_to_opkind(NodeTag pathtype);

/* planbuild.c */
extern Plan *pg_cascades_extract_best_plan(PgPlannerCascadesContext *ctx);

/* postopt.c */
extern void pg_cascades_validate_plan(Plan *plan);
extern Plan *pg_cascades_physical_rewrite(PgPlannerCascadesContext *ctx,
                                           Plan *plan);

/* planmain.c — split query_planner for Cascades */
extern QueryPlannerPrepResult *prepare_query_planner_inputs(
    PlannerInfo *root, List *tlist,
    double tuple_fraction, double limit_tuples);
extern void finish_query_planner_after_prepare(
    PlannerInfo *root, QueryPlannerPrepResult *prep,
    List *tlist, double tuple_fraction, double limit_tuples,
    Path **cheapest_path, Path **sorted_path, double *num_groups);

/* decorrelate.c (Phase 6) */
extern bool pg_cascades_decorrelate_subqueries(PlannerInfo *root);

/* rewrite.c */
extern PgCascadesStatus pg_cascades_logical_rewrite(
    PgPlannerCascadesContext *ctx);

/* Phase 4: Tree-based rewrite operating on OptExpression tree */
extern PgCascadesStatus pg_cascades_logical_rewrite_v2(
    PgPlannerCascadesContext *ctx, PgGroupExpr *tree_root);

/* Phase 4: OptExpression tree for pre-Memo rewrite */
extern PgGroupExpr *pg_cascades_build_initial_tree(
    PgPlannerCascadesContext *ctx);

/* Phase 4: logical property derivation per operator */
extern void pg_memo_derive_logical_property_v2(PgMemo *memo,
    PgPlannerCascadesContext *ctx);

/* Phase 4: combination rule registration */
extern void pg_cascades_init_combination_rules(void);
extern PgCombinationRule *pg_cascades_get_combination_rules(int *num_rules);

/* debug.c */
extern void debug_print_cascades_memo(PgPlannerCascadesContext *ctx);
extern void debug_print_cascades_rules(PgPlannerCascadesContext *ctx);
extern void debug_print_cascades_fallback_reason(
    PgPlannerCascadesContext *ctx, const char *reason);

#endif /* CASCADES_H */
