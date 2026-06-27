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
subquery_planner()
  ├─ pull_up_sublinks()          # IN/EXISTS → semi/anti join
  └─ grouping_planner()           # ★ Cascades 接入点
       │
       ├─ [precheck] 支持性检查
       │    ├─ 仅 SELECT（非 DML）
       │    ├─ 无 Window / CTE / 递归 / 继承表 / FDW
       │    ├─ UNION ALL 允许，其他 SETOP fallback
       │    └─ 无相关子查询（SubPlan with parParam）
       │
       ├─ make_one_rel()           # PG 生成下层 scan/join Path
       │
       ├─ pg_memo_init_from_tree() ★ 构建 Memo
       │    ├─ 1. 创建 Memo + 哈希表
       │    ├─ 2. 构建 OptExpression 树（Query → 树）
       │    └─ 3. 将树插入 Memo（Group + GroupExpression）
       │
       ├─ pg_memo_derive_logical_property()  # 逻辑属性推导
       │
       ├─ pg_cascades_logical_rewrite()      # 8 阶段改写流水线
       │
       ├─ pg_cascades_run_tasks()            # TaskScheduler 优化搜索
       │
       ├─ pg_cascades_extract_best_plan()    # 提取最优 Plan
       │    └─ 两阶段提取：COMPOSABLE_OP 优先 → IMPORTED_PATH 回退
       │
       ├─ pg_cascades_validate_plan()        # 结构校验
       ├─ pg_cascades_physical_rewrite()     # Material 节点插入
       │
       └─ copyObject(plan) → 返回 Plan *
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

### 3.1 步骤 1：支持性检查

`pg_cascades_supported_query_precheck()` 做快速粗筛：

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

**UNION ALL 特殊处理**：UNION ALL 不需要去重，可作为 Append 计划处理，允许通过 precheck。

### 3.2 步骤 2：构建下层路径

调用 PG 原生的 `make_one_rel(root, joinlist)` 生成所有 scan/join Path。

**关键细节**：在调用前切换到 PG 的 planner context，否则 Path 会被分配在 Cascades 的 memo context 中——fallback 时访问已释放内存会 SIGSEGV。

### 3.3 步骤 3：构建 Memo

`pg_memo_init_from_tree()` 是 StarRocks `Memo.init()` 的等价函数：

1. **创建 Memo 壳**：`palloc0` Memo + `hash_create` 去重哈希表
2. **构建 OptExpression 树**：`pg_cascades_build_initial_tree()` 将 PG 的 Query + joinlist 转为逻辑表达式树：
   ```
   LogicalLimit → LogicalSort → LogicalDistinct → LogicalAgg
              → LogicalProject → LogicalJoin(s) → LogicalScan(s)
   ```
3. **插入 Memo**：`pg_memo_insert_expression_tree()` 递归将树转为 Group + GroupExpression。对 LogicalScan 导入 PG 的 Path 作为 IMPORTED_PATH 物理候选；对 LogicalJoin 查找 `join_rel_list` 匹配导入 join Path。

### 3.4 步骤 4：逻辑属性推导

`pg_memo_derive_logical_property()` 自底向上计算每个 Group 的 rows/width。同时填充 `PgStatistics` 结构体。

### 3.5 步骤 5：改写流水线

`pg_cascades_logical_rewrite()` 在 Memo Group 上应用变换规则，共 9 个阶段：

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

### 3.6 步骤 6：TaskScheduler 优化

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

### 3.7 步骤 7：计划提取

`pg_cascades_extract_best_plan()` 两阶段提取：

1. **Pass 0：COMPOSABLE_OP 优先**——尝试从 COMPOSABLE_OP entry 递归构建 Plan（`build_plan_recurse` 进入 PHYSICAL_SORT/AGG/LIMIT 等分支）
2. **Pass 1：IMPORTED_PATH 回退**——如果 COMPOSABLE_OP 提取失败（如 join 没有 Path*），用 IMPORTED_PATH 的 `create_plan(path)` 构建

对于 Scan/Join：`build_plan_recurse` 不处理 COMPOSABLE_OP 的 scan/join（返回 NULL），由 IMPORTED_PATH 处理。
对于上层算子：COMPOSABLE_OP entry 递归构建——Sort → `make_sort_from_pathkeys`、Agg → `make_agg`、Limit → `make_limit` 等。

### 3.8 步骤 8-9：后优化 + 清理

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

| 特性 | StarRocks | PG Cascades |
|------|----------|------------|
| 优化器语言 | Java | C |
| 规则总数 | 198 | ~40 |
| Join 枚举 | 完整（bushy trees） | Path 导入模式（Phase 2 设计就位） |
| 统计信息 | 列级 + 直方图 | 行级（RelOptInfo.rows/width） |
| 分布式属性 | DistributionProperty | 不需要（单机） |
| 改写流水线 | 组合规则系统 | 8 阶段流水线 |
| Window/CTE/SETOP | 完整支持 | fallback（UNION ALL 除外） |
| 代价模型 | CPU/Memory/Network | PG costsize.c |
| 代码量 | ~10 万行 Java | ~8200 行 C |

---

## 十、下一步计划

| 优先级 | 功能 | 状态 |
|--------|------|------|
| ~~P1~~ | Statistics 对象 | ✅ 完成 |
| ~~P1~~ | Join Enumeration (Phase 1) | ✅ 完成（代价公平竞争） |
| P2 | Join Enumeration (Phase 2) | 设计就位，需变换规则创建新 join 组合 |
| P2 | JoinAssociativity 启用 | 规则已注册，需验证正确性 |
| P3 | Window / CTE / 完整 SETOP | fallback |
| — | 覆盖率 → 80% | 需 planbuild 代码变更 |
