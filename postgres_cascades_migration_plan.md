# PostgreSQL Cascades 优化器 — 设计与实现文档

> 基于 StarRocks Cascades 框架，迁移到 PostgreSQL 9.2.4。
> 本文档与 `cascade_dev` 分支代码完全一致。最后更新：2026-06-27。

---

## 一、概述：我们做了什么

PostgreSQL 9.2.4 原生的查询优化器使用自底向上的动态规划（`standard_join_search`），无法做全局最优选择。我们从 StarRocks 引入了 Cascades 框架，在 PG 的 `grouping_planner` 中接入了一个新的优化分支，实现了**上层算子（Agg/Sort/Limit/Distinct/Project）的全局优化**。

### 一句话总结

**用 Cascades 的 Memo + TaskScheduler + Rule 系统，替代 PG 原生的上层 plan 包装逻辑。** 下层 scan/join 仍复用 PG 成熟的路径生成（`make_one_rel`），以 IMPORTED_PATH 形式导入 Memo。

### 关键数字

| 指标 | 数值 |
|------|------|
| 代码量 | 11 个 C 源文件 + 1 个头文件，共 ~8200 行 |
| 逻辑算子 | 9 种（Scan/Filter/Project/Join/Agg/Distinct/Sort/Limit/Union） |
| 物理算子 | 15 种（涵盖所有 scan/join/upper ops） |
| 实现规则 | 18 条（逻辑→物理转换） |
| 变换规则 | 25+ 条（逻辑→逻辑改写） |
| 测试用例 | 573 个（372 主 + 201 专项），0 失败，0 fallback |
| 行覆盖率 | **74.5%** |
| 函数覆盖率 | **91.2%** |

---

## 二、架构全景

### 2.1 整体流程

```
SQL 查询
  ↓
PG Parser / Analyzer / Rewriter
  ↓
standard_planner()
  └─ subquery_planner()
       ├─ SS_process_ctes()         # CTE → InitPlan（★ Cascades 拦不住）
       ├─ pull_up_sublinks()        # IN/EXISTS → semi/anti join（★ 拦不住）
       ├─ pull_up_subqueries()      # FROM 子查询提升（★ 拦不住）
       ├─ flatten_simple_union_all()
       ├─ expand_inherited_tables()
       ├─ preprocess_expression()   # 表达式规范化
       │
       └─ grouping_planner()        # ★ Cascades 接入点（太晚了）
            │
            ├─ ① pg_cascades_supported_query_precheck()   # 语义粗筛
            │     · 仅 SELECT / 无 Window / 无递归 CTE
            │     · 无关联 SubPlan / UNION ALL 放行
            │
            ├─ ② prepare_query_planner_inputs()           # 建基础设施
            │     · setup_simple_rel_arrays() → 建 base RelOptInfo
            │     · deconstruct_jointree()  → 展开 join tree → joinlist
            │     · generate_base_implied_equalities() → 等价类
            │
            ├─ ③ pg_cascades_supported_query()            # 结构检查
            │     · 每个 base rel 检查 rtekind / relkind / FDW
            │
            └─ ④ pg_cascades_try_grouping_planner()       # ★ 真正的主入口
                 │
                 ├─ 1. 创建 MemoryContext (PgCascadesMemo)
                 ├─ 2. 初始化规则 (pattern + 5 组合并 + 排序)
                 │
                 ├─ 3. make_one_rel(root, joinlist)        # PG 生成下层 Path
                 │      · set_rel_pathlist() → 各 scan/join 路径
                 │      · 结果存入 prep->final_rel
                 │
                 ├─ 4. pg_memo_init_from_tree(&ctx)        # 构建 Memo
                 │      · 创建 Memo + hash 去重表
                 │      · Query → OptExpression 树
                 │      · 插入 Memo（Group + GroupExpression）
                 │
                 ├─ 5. pg_memo_derive_logical_property()   # 逻辑属性推导
                 ├─ 6. pg_cascades_logical_rewrite()       # 8 阶段改写
                 ├─ 7. pg_memo_derive_logical_property_v2()# 改写后重推导
                 ├─ 8. pg_cascades_run_tasks()             # TaskScheduler 搜索
                 ├─ 9. pg_cascades_extract_best_plan()     # 提取最优 Plan
                 ├─ 10. pg_cascades_validate_plan()        # 结构校验
                 ├─ 11. pg_cascades_physical_rewrite()     # Material 插入
                 │
                 └─ 12. copyObject(plan) → MemoryContextDelete
  ↓
grouping_planner 后续: apply_scanjoin_target / sort / limit ...
  ↓
set_plan_references() → PlannedStmt → Executor
```

### 2.2 核心数据结构

```
PgMemo                    # 搜索空间容器
  ├─ groups: List<PgMemoGroup>
  ├─ group_expr_table: HTAB    # 去重哈希表
  └─ root_group: PgMemoGroup

PgMemoGroup               # 等价结果集合
  ├─ id: int
  ├─ logical_exprs: List<PgGroupExpr>
  ├─ physical_exprs: List<PgGroupExpr>
  ├─ best_entries: List<PgGroupBestEntry>  # 属性→最优表达式
  ├─ rows / width: double / int
  ├─ stats: PgStatistics                    # Phase 6 结构化统计
  └─ rel: RelOptInfo *                      # 对应 PG 关系

PgGroupExpr               # 一种计算方式
  ├─ op: PgCascadesOpKind
  ├─ mode: IMPORTED_PATH | COMPOSABLE_OP
  ├─ inputs: List<PgMemoGroup *>
  ├─ op_private: void *     # Path* | PgJoinPrivate* | ...
  └─ explored_rules: Bitmapset

PgRequiredProperty        # 父节点对子节点的物理属性要求
  ├─ pathkeys: List *        # 排序要求（NIL=无要求）
  ├─ required_outer: Relids  # 参数化路径依赖
  ├─ tuple_fraction: double  # row goal
  └─ limit_tuples: double

PgGroupBestEntry          # 某属性下的最优表达式
  ├─ required: PgRequiredProperty *
  ├─ expr: PgGroupExpr *
  ├─ startup_cost / total_cost: Cost
  ├─ child_required_props: List<PgRequiredProperty *>
  └─ output: PgOutputProperty
```

### 2.3 两种物理表达式模式

| 模式 | op_private | inputs | 使用场景 |
|------|-----------|--------|---------|
| **IMPORTED_PATH** | `Path *`（PG 生成的完整路径树） | NIL | Scan/Join（复用 `make_one_rel` 输出） |
| **COMPOSABLE_OP** | 算子参数（如 `PgJoinPrivate*`） | `List<PgMemoGroup*>` | Upper ops（Agg/Sort/Limit等）和 join |

**设计决策**：PG 已有成熟的 scan/join path 生成（20+ 年打磨），我们没有重新实现。IMPORTED_PATH 直接导入 PG 的 Path 树，cost 从 `Path.startup_cost/total_cost` 读取。COMPOSABLE_OP 用于 PG 没有的上层算子（PG 没有 upper Path），由 Cascades 自己构建。

---

## 三、执行流程详解

### 3.0 接入时机：为什么是 `grouping_planner` 内部？

Cascades 通过 GUC `enable_cascades_planner` 在 PG 的 `grouping_planner()` 中分支。接入点之前，`subquery_planner()` 已经做了以下不可逆的操作：

| 步骤 | 函数 | 效果 | Cascades 能拦截？ |
|------|------|------|------------------|
| CTE 处理 | `SS_process_ctes()` | CTE 固化为 InitPlan + CteScan | ❌ 不能 |
| SubLink 转换 | `pull_up_sublinks()` | ANY/EXISTS/IN → SemiJoin | ❌ 不能 |
| 子查询提升 | `pull_up_subqueries()` | FROM 子查询提升到主查询 | ❌ 不能 |
| UNION ALL 展开 | `flatten_simple_union_all()` | UNION ALL 转为 appendrel | ❌ 不能 |
| 继承表展开 | `expand_inherited_tables()` | 分区表展开 | ❌ 不能 |
| 表达式预处理 | `preprocess_expression()` | 类型推导、常量折叠 | ❌ 不能 |

**这意味着**：Cascades 拿到的是 PG 已经做过一轮逻辑优化的"残局"。要支持 CTE/子查询，只能在 Memo 内对残余物做补救（如相关 SubPlan → Apply 算子），而不能重做 PG 已经完成的优化。

### 3.1 步骤 ①-③：三层检查 + 基础设施

在进入 `pg_cascades_try_grouping_planner()` 之前，`grouping_planner` 内部有三层串行调用：

**① `pg_cascades_supported_query_precheck()`** — 语义级粗筛（[cascades.c:98](postgres/src/backend/optimizer/cascades/cascades.c#L98)）：

| 检查项 | 不支持的 | 处理 |
|--------|---------|------|
| 命令类型 | 非 SELECT | fallback |
| 集合操作 | UNION/INTERSECT/EXCEPT | fallback（**UNION ALL 除外**） |
| 窗口函数 | 有 | fallback |
| CTE | 有（递归/修改） | fallback |
| 行锁 | FOR UPDATE/SHARE | fallback |
| DISTINCT ON | 有 | fallback |
| 聚合 | MIN/MAX 特殊优化 | fallback |
| 子查询 | 相关 SubPlan | fallback |

**② `prepare_query_planner_inputs()`** — 建 PG 查询基础设施（[planner.c:1260](postgres/src/backend/optimizer/cascades/../plan/planner.c#L1260)）：
- `setup_simple_rel_arrays()` → 为每个 RTE 建 `RelOptInfo`
- `deconstruct_jointree()` → 展开 join tree → 产生 `joinlist`（后续 `make_one_rel` 的输入）
- `generate_base_implied_equalities()` → 等价类推导

**③ `pg_cascades_supported_query()`** — 结构级检查（[cascades.c:138](postgres/src/backend/optimizer/cascades/cascades.c#L138)）：
- 检查每个 base rel 的 `rtekind == RTE_RELATION`（普通表）→ CTE（RTE_CTE）和子查询（RTE_SUBQUERY）在此被挡
- 检查 `relkind` / FDW / 继承表
- Phase 1 限制：3+ 表 JOIN + ORDER+LIMIT → fallback

**只有三步全部返回 OK，才进入真正的 Cascades 主入口。**

### 3.2 主入口：`pg_cascades_try_grouping_planner()`

12 步全流程（[cascades.c:248-528](postgres/src/backend/optimizer/cascades/cascades.c#L248-L528)）：

**Step 1-2. 环境初始化**
- 创建专用 `MemoryContext` (`PgCascadesMemo`)
- 初始化 rule patterns + 合并 5 组规则（Phase 1 impl + Phase 2 scan/join + Phase 4 join + Enforcer + Phase 3/5 trans），按 promise 降序排列
- 检查 `trivial_result`（无 FROM 的查询）→ bail out

**Step 3. `make_one_rel(root, joinlist)`** — PG 生成下层路径
- 调用 PG 原生的 `make_one_rel()` 生成所有 scan/join Path
- **关键细节**：在调用前切换到 PG 的 planner context，否则 Path 会被分配在 Cascades 的 memo context 中——fallback 时访问已释放内存会 SIGSEGV
- 结果存入 `prep->final_rel`

**Step 4. `pg_memo_init_from_tree()`** — 构建 Memo（StarRocks `Memo.init()` 等价函数）

1. **创建 Memo 壳**：`palloc0` Memo + `hash_create` 去重哈希表
2. **构建 OptExpression 树**：`pg_cascades_build_initial_tree()` 将 PG 的 Query + joinlist 转为逻辑表达式树：
   ```
   LogicalLimit → LogicalSort → LogicalDistinct → LogicalAgg
              → LogicalProject → LogicalJoin(s) → LogicalScan(s)
   ```
3. **插入 Memo**：`pg_memo_insert_expression_tree()` 递归将树转为 Group + GroupExpression。对 LogicalScan 导入 PG 的 Path 作为 IMPORTED_PATH 物理候选；对 LogicalJoin 查找 `join_rel_list` 匹配导入 join Path。

**Step 5. `pg_memo_derive_logical_property()`** — 逻辑属性推导
- 自底向上计算每个 Group 的 rows/width。同时填充 `PgStatistics` 结构体。

**Step 6. `pg_cascades_logical_rewrite()`** — 8 阶段改写流水线
- 在 Memo Group 上应用变换规则，详见 [3.3](#33-步骤-6改写流水线)

**Step 7. `pg_memo_derive_logical_property_v2()`** — 改写后重新推导
- 改写可能产生新 group（如 JoinAssociativity 创建新 join），需要重新计算属性

**Step 7b. Root group 修正** — 改写可能把 root group 清空（合并到其他 group），遍历找到第一个非空 group 作为新 root

**Step 8. `pg_cascades_run_tasks()`** — TaskScheduler 优化搜索，详见 [3.4](#34-步骤-8taskscheduler-优化)

**Step 9. `pg_cascades_extract_best_plan()`** — 提取最优 Plan，详见 [3.5](#35-步骤-9计划提取)

**Step 10. `pg_cascades_validate_plan()`** — 结构校验，递归检查 Plan 树完整性

**Step 11. `pg_cascades_physical_rewrite()`** — 物理改写，在 NestLoop 内侧插入 Material 节点

**Step 12. 清理**
- `copyObject(plan)` 将 Plan 从 MemoContext 复制到调用方 context
- `MemoryContextDelete(memo_cxt)` 释放所有 Cascades 内存
- 返回 status → 调用方根据 status 决定 fallback 或使用 Cascades plan

### 3.3 步骤 6：改写流水线

`pg_cascades_logical_rewrite()` 在 Memo Group 上应用变换规则，共 8 个阶段（CTE Inline 和 Subquery 阶段当前为空占位）：

| 阶段 | 规则数 | 遍历方向 | 说明 |
|------|--------|---------|------|
| Predicate Pushdown | 4 | bottom-up | 将 WHERE 条件下推到 Scan |
| Column Pruning | 5 | **top-down** | 从上往下裁剪不需要的列 |
| Join Reorder | 3 | bottom-up | JoinCommutativity（INNER JOIN 交换） |
| Limit Push | 2 | bottom-up | MergeLimitWithSort 等 |
| Aggregate Pushdown | 2 | bottom-up | PushDownAggLimit, MergeTwoAgg |
| Semi-Join Dedup | 7 | bottom-up | InnerToSemi, EliminateJoin 等 |
| Final Cleanup | 7 | bottom-up | EliminateProject/Limit/Agg, PruneEmpty 等 |

每阶段迭代至收敛（最多 10 轮），列裁剪用 top-down 遍历（上层需求向下传播）。

### 3.4 步骤 8：TaskScheduler 优化

`pg_cascades_run_tasks()` 是 LIFO 栈驱动的主循环。6 种任务：

```
OPTIMIZE_GROUP           ← 入口
  ├─ [push] ENFORCE_AND_COST  × 每个 physical expr × 4 种 pathkeys
  ├─ [push] OPTIMIZE_EXPRESSION × 每个 logical expr
  └─ [push] OPTIMIZE_GROUP(child)  # LIFO → 子节点先执行
       │
OPTIMIZE_EXPRESSION
  ├─ [push] APPLY_RULE × 每个匹配的实现规则
  ├─ [push] DERIVE_STATS
  └─ [push] EXPLORE_GROUP(child)
       │
APPLY_RULE
  ├─ 模式匹配（如果 rule->pattern != NULL）
  ├─ transform() → 新表达式
  ├─ 插入 Memo（哈希去重 + Group 自动合并）
  ├─ 实现规则 → [push] ENFORCE_AND_COST
  └─ 变换规则 → [push] OPTIMIZE_EXPRESSION
       │
ENFORCE_AND_COST           ← 4 状态机
  ├─ ENFORCE_INIT            → 推导 child required properties
  ├─ ENFORCE_OPTIMIZE_CHILDREN → Clone+Resume（子节点未就绪时）
  ├─ ENFORCE_COMPUTE_COST    → 计算代价，更新 group.best_entries
  └─ ENFORCE_ENFORCE_PROPERTY → 插入 Sort enforcer
```

**关键机制**：

- **LIFO 自底向上**：子 Group 的 OPTIMIZE_GROUP 最后 push → 最先执行
- **多属性优化**：每个 physical expr 推 4 个 ENFORCE_AND_COST 任务（NIL/sort/group/distinct pathkeys）
- **Clone+Resume**：子节点对特定 required property 未就绪时，克隆自身入栈，先优化子节点
- **代价下界剪枝**：`lower_bound_cost > upper_bound_cost` → 整组跳过
- **全局上界剪枝**：`child_total + local_cost > ctx->upper_bound_cost` → 跳过

### 3.5 步骤 9：计划提取

`pg_cascades_extract_best_plan()` 两阶段提取：

1. **Pass 0：COMPOSABLE_OP 优先**——尝试从 COMPOSABLE_OP entry 递归构建 Plan（`build_plan_recurse` 进入 PHYSICAL_SORT/AGG/LIMIT 等分支）
2. **Pass 1：IMPORTED_PATH 回退**——如果 COMPOSABLE_OP 提取失败（如 join 没有 Path*），用 IMPORTED_PATH 的 `create_plan(path)` 构建

对于 Scan/Join：`build_plan_recurse` 不处理 COMPOSABLE_OP 的 scan/join（返回 NULL），由 IMPORTED_PATH 处理。
对于上层算子：COMPOSABLE_OP entry 递归构建——Sort → `make_sort_from_pathkeys`、Agg → `make_agg`、Limit → `make_limit` 等。

### 3.6 步骤 10-12：后优化 + 清理

- `pg_cascades_validate_plan()`：递归检查 Plan 树结构完整性
- `pg_cascades_physical_rewrite()`：在 NestLoop 内侧插入 Material 节点
- `copyObject(plan)`：将 Plan 复制到调用方 context
- `MemoryContextDelete(memo_cxt)`：清理所有 Cascades 内存

---

## 四、核心设计决策

### 4.1 IMPORTED_PATH vs COMPOSABLE_OP

这是 PG 端口最核心的设计决策。

| 方面 | IMPORTED_PATH | COMPOSABLE_OP |
|------|-------------|--------------|
| **适用** | Scan / Join | Upper ops (Agg/Sort/Limit/Project) + Join |
| **来源** | PG `make_one_rel()` | Cascades 实现规则 |
| **代价** | 从 `Path.startup_cost/total_cost` 直读 | PG 成本公式计算 |
| **提取** | `create_plan(path)` | 递归构建 Plan + PG `make_*` 函数 |

**为什么不全部用 COMPOSABLE_OP？** PG 的 scan/join path 生成有 20+ 年优化历史，重写会丢失这些优化。IMPORTED_PATH 是务实的选择。

**为什么上层用 COMPOSABLE_OP？** PG 没有上层 Path（Agg/Sort/Limit 没有对应的 Path 类型），必须自己构建。

### 4.2 Join Enumeration 策略

当前 Phase 1 使用 **Path 导入模式**：join 顺序由 PG 的 `standard_join_search` 决定，Cascades 从 PG 生成的候选中选择最优。

Phase 2（设计就位，代码标志已设）将引入真正的 Cascades join 枚举：
- 通过 JoinCommutativity/Associativity 变换规则创建新的 join 组合
- COMPOSABLE_OP join 使用完整的 PG 成本公式（无惩罚分）
- `make_join_rel()` 验证 join 合法性

### 4.3 Outer Join 建模

通过 PG 的 `SpecialJoinInfo` 推导实际 join 类型：

```
deconstruct_jointree 产生的 join_info_list
  ↓
pg_determine_join_type(left_relids, right_relids)
  ↓
JOIN_INNER / JOIN_LEFT / JOIN_RIGHT / JOIN_SEMI / JOIN_ANTI
```

改写规则通过 `pg_join_is_commutable()` 守卫（仅 INNER JOIN 可交换）。

### 4.4 统计数据

`PgStatistics { row_count, width, derived }` 嵌入 `PgMemoGroup`，提供结构化统计。
- `pg_statistics_from_group()` — 从 Group 提取统计
- `pg_statistics_derive()` — 从 GroupExpression 推导统计
- `pg_derive_expr_stats()` — 使用 PG `clauselist_selectivity` 估算过滤后的 rows

### 4.5 代价下界剪枝（StarRocks 对齐）

```
OPTIMIZE_GROUP 入口:
  group->lower_bound_cost > ctx->upper_bound_cost → 整组跳过

ENFORCE_AND_COST:
  IMPORTED_PATH: 更新 group->lower_bound_cost = min(path->total_cost)
  COMPOSABLE_OP:  更新 group->lower_bound_cost = min(child_sum)
  child_group->lower_bound_cost > ctx->upper_bound_cost → 跳过子节点
```

---

## 五、支持的 SQL 能力

| SQL 特性 | 支持 | 说明 |
|---------|------|------|
| SELECT ... FROM t | ✅ | 单表查询 |
| WHERE 过滤 | ✅ | AND/OR/IN/LIKE/BETWEEN/IS NULL |
| SeqScan / IndexScan / BitmapHeapScan | ✅ | PG 路径导入 |
| INNER JOIN | ✅ | NestLoop / HashJoin / MergeJoin |
| LEFT / RIGHT JOIN | ✅ | SpecialJoinInfo 推导 |
| SEMI / ANTI JOIN | ✅ | EXISTS / NOT EXISTS 被 pull_up_sublinks 转换后 |
| GROUP BY | ✅ | HashAgg / GroupAgg |
| HAVING | ✅ | |
| ORDER BY | ✅ | Sort enforcer |
| LIMIT / OFFSET | ✅ | |
| DISTINCT | ✅ | Unique / HashAgg |
| 聚合函数 (COUNT/SUM/AVG/MIN/MAX) | ✅ | 复用 PG Aggref |
| 标量子查询 | ⚠️ | 非相关的通过 initPlan，相关的 fallback |
| **UNION ALL** | ✅ | Append 计划，单独放行 |
| UNION / INTERSECT / EXCEPT | ❌ | fallback |
| Window Function | ❌ | fallback |
| CTE（递归/修改） | ❌ | fallback |
| DISTINCT ON | ❌ | fallback |
| FULL JOIN | ❌ | fallback |
| FDW/外部表 | ❌ | fallback |
| 继承表/分区表 | ❌ | fallback |
| FOR UPDATE/SHARE | ❌ | fallback |
| MIN/MAX 特殊聚合优化 | ❌ | fallback |

---

## 六、文件清单

| 文件 | 行数 | 覆盖率 | 职责 |
|------|------|--------|------|
| `cascades.h` | 701 | — | 所有数据结构 + 函数声明 |
| `cascades.c` | 528 | 83.4% | 主入口 + 支持性检查 + fallback |
| `memo.c` | 1053 | 74.0% | Memo/Group/GroupExpression + 统计 |
| `task.c` | 1357 | 81.5% | TaskScheduler (6 种 Task) |
| `rule.c` | 2703 | 68.9% | 规则注册表 (18+25 条规则) |
| `property.c` | 309 | 83.8% | Property 操作 + 子属性推导 |
| `pattern.c` | 359 | 74.8% | Pattern 匹配引擎 |
| `pg_adapter.c` | 304 | 86.4% | PG 适配：Query → OptExpression |
| `planbuild.c` | 667 | 58.2% | Plan 提取 + 两阶段回退 |
| `rewrite.c` | 624 | 90.1% | 8 阶段改写流水线 |
| `postopt.c` | 255 | 43.9% | Plan 校验 + Material 插入 |
| `debug.c` | 74 | 100% | 调试输出 |
| **总计** | **~8200** | **74.5%** | |

---

## 七、规则清单

### 7.1 实现规则（18 条）

| 规则 | 从 | 到 | 模式 |
|------|----|----|------|
| Agg→HashAgg | LOGICAL_AGG | PHYSICAL_HASHAGG | COMPOSABLE_OP |
| Agg→GroupAgg | LOGICAL_AGG | PHYSICAL_GROUPAGG | COMPOSABLE_OP |
| Sort→Sort | LOGICAL_SORT | PHYSICAL_SORT | COMPOSABLE_OP |
| Distinct→Unique | LOGICAL_DISTINCT | PHYSICAL_UNIQUE | COMPOSABLE_OP |
| Limit→Limit | LOGICAL_LIMIT | PHYSICAL_LIMIT | COMPOSABLE_OP |
| Project→Project | LOGICAL_PROJECT | PHYSICAL_PROJECT | COMPOSABLE_OP |
| Scan→SeqScan | LOGICAL_SCAN | PHYSICAL_SEQSCAN | COMPOSABLE_OP |
| Scan→IndexScan | LOGICAL_SCAN | PHYSICAL_INDEXSCAN | COMPOSABLE_OP |
| Scan→BitmapHeapScan | LOGICAL_SCAN | PHYSICAL_BITMAP_HEAPSCAN | COMPOSABLE_OP |
| Join→NestLoop | LOGICAL_JOIN | PHYSICAL_NESTLOOP | COMPOSABLE_OP |
| Join→HashJoin | LOGICAL_JOIN | PHYSICAL_HASHJOIN | COMPOSABLE_OP |
| Join→MergeJoin | LOGICAL_JOIN | PHYSICAL_MERGEJOIN | COMPOSABLE_OP |
| Join→HashJoin(Phase4) | LOGICAL_JOIN | PHYSICAL_HASHJOIN | IMPORTED_PATH |
| Join→NestLoop(Phase4) | LOGICAL_JOIN | PHYSICAL_NESTLOOP | IMPORTED_PATH |
| Join→MergeJoin(Phase4) | LOGICAL_JOIN | PHYSICAL_MERGEJOIN | IMPORTED_PATH |
| Sort Enforcer | (enforcer) | PHYSICAL_SORT | COMPOSABLE_OP |

### 7.2 变换规则（25+ 条）

**谓词下推**：PushDownPredicateScan / Join / Project / Agg
**列裁剪**：PruneScanColumns / JoinColumns / AggColumns / ProjectColumns / SortColumns
**Join 重排**：JoinCommutativity
**Limit 优化**：MergeLimitWithSort / PushDownLimitJoin / EliminateLimit / MergeLimitWithChildLimit
**聚合优化**：PushDownAggLimit / MergeTwoAgg / EliminateAgg
**Semi-Join**：InnerToSemi / EliminateJoinWithConst / OuterJoinElimination
**Merge 消除**：MergeProjectWithChild / EliminateProject / MergeFilterWithJoin / MergeJoinWithChildProj
**空集裁剪**：PruneEmptyScan / PruneEmptyJoin / EliminateSortWithConstKey

---

## 八、改造顺序回顾

| 阶段 | 覆盖率 | 主要变更 |
|------|--------|---------|
| 基线 | 59.5% | 原始代码 |
| Gap 1-4 | 61.0% | Outer Join 建模 + DERIVE_STATS + COMPOSABLE_OP join 代价 + Join 合法性守卫 |
| SQL 测试 | 63.5% | 201 个专项测试 + JOIN_REORDER 启用 + debug=on |
| 死代码 R1 | 66.3% | 删除 build_logical_plan / find_imported_path |
| P0-2+P1 | 67.9% | 多属性任务 + pattern-based 规则 |
| 死代码 R2 | 70.1% | 删除 rewrite_v2 + pg_memo_init + fallback_reason |
| 更多测试+守卫 | 72.7% | UNION 死代码 + 多节点 Binder + SQL 扩展 |
| 守卫精简+两级提取 | 73.4% | planbuild COMPOSABLE_OP 优先 + ~100 行守卫删除 |
| decorrelate 删除 | 75.0% | 脚手架移除 |
| P1 Stats + Join | **74.5%** | PgStatistics 嵌入 + COMPOSABLE_OP 公平代价 |

---

## 九、StarRocks 对比

| 特性 | StarRocks | PG Cascades | 差距 |
|------|----------|------------|------|
| 优化器语言 | Java | C | — |
| 规则总数 | 198 | ~40 | 量级差距，已覆盖 Phase 1 核心规则 |
| **JoinCommutativity** | ✅ A⋈B→B⋈A | ✅ 已启用（rewrite pipeline） | **无差距** |
| **JoinAssociativity** | ✅ (A⋈B)⋈C→A⋈(B⋈C) | ⚠️ 规则存在，transform 被守卫拦截 | **有差距**：无法重组多表 join 树 |
| Join 物理算子生成 | ✅ 实现规则生成 | ✅ Phase4 调用 `make_join_rel`→IMPORTED_PATH | **无差距** |
| COMPOSABLE_OP join 代价 | ✅ 完整代价模型 | ✅ PG 公式，与 IMPORTED_PATH 公平竞争 | **无差距** |
| Join 顺序发现 | ✅ 从平表列表构建所有树 | ⚠️ 初始树由 PG `deconstruct_jointree` 决定 | **有差距**：仅可交换，不可发现新顺序 |
| 统计信息 | 列级 + 直方图 | 行级（RelOptInfo.rows/width + PgStatistics） | Phase 2 |
| 分布式属性 | DistributionProperty | 不需要（单机） | — |
| 改写流水线 | 组合规则系统 | 8 阶段流水线 | 覆盖核心场景 |
| Window/CTE/SETOP | 完整支持 | fallback（UNION ALL 除外） | Phase 3 |
| 代价模型 | CPU/Memory/Network | PG costsize.c | PG 单机模型更准确 |
| 代码量 | ~10 万行 Java | ~8200 行 C |

---

## 十、下一步计划

| 优先级 | 功能 | 状态 | 说明 |
|--------|------|------|------|
| ~~P1~~ | Statistics 对象 | ✅ | `PgStatistics` 嵌入 `PgMemoGroup` |
| ~~P1~~ | JoinCommutativity + 公平代价 | ✅ | 可交换 2 表 join，COMPOSABLE_OP 无惩罚分 |
| **P2** | **JoinAssociativity 启用** | ⚠️ **主要差距** | 规则存在但 `pg_rule_join_associativity` 返回 NIL。需修复守卫条件 + 实现 (A⋈B)⋈C → A⋈(B⋈C) 变换。启用后可与 StarRocks join 枚举对齐 |
| P3 | Window / CTE / 完整 SETOP | ❌ | fallback |
| — | 覆盖率 → 80% | — | 需 planbuild 代码变更 |
