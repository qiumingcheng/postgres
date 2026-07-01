# 3-Way JOIN 失败的根本原因分析与修复方案

## 问题 SQL

```sql
SELECT j1.a, j2.d, j3.f 
FROM cov_j1 j1 
JOIN cov_j2 j2 ON j1.a = j2.a 
JOIN cov_j3 j3 ON j1.a = j3.a;
```

**错误**：`variable not found in subplan target list`

---

## PG 原生计划器的正确执行流程

### 1. grouping_planner() 阶段

```c
// planner.c:1114
sub_tlist = make_subplanTargetList(root, tlist, &groupColIdx, &need_tlist_eval);
```

**make_subplanTargetList() 的职责**：
- 输入：最终 SELECT 的 tlist = [j1.a, j2.d, j3.f]
- 输出：sub_tlist（传递给 query_planner 的目标列表）
- **关键**：sub_tlist 包含所有需要的列，包括中间 JOIN 条件列

**推测 sub_tlist 的内容**：
```
[j1.a, j2.d, j3.f]  // 初步，可能还不完整
```

### 2. query_planner() 阶段

```c
// planmain.c
final_rel = make_one_rel(root, joinlist);
```

#### 2.1 deconstruct_jointree()

- 分析 JOIN 树结构
- 提取所有 JOIN 条件中的 Var
- 调用 `add_vars_to_targetlist()` 将这些 Var 添加到对应 base relation 的 `reltargetlist`

**示例**：
- JOIN 条件 1：`j1.a = j2.a` → 需要 j1.a, j2.a
- JOIN 条件 2：`j1.a = j3.a` → 需要 j1.a, j3.a

**add_vars_to_targetlist() 的效果**：
```c
j1->reltargetlist = [j1.a]
j2->reltargetlist = [j2.a, j2.d]  // j2.a 用于 JOIN，j2.d 用于输出
j3->reltargetlist = [j3.a, j3.f]  // j3.a 用于 JOIN，j3.f 用于输出
```

#### 2.2 make_one_rel()

- 基于 base relation 的 reltargetlist 构建 JOIN 路径
- 生成 `final_rel`（j1 ⋈ j2 ⋈ j3 的结果）
- **关键**：`final_rel->reltargetlist` 包含所有必要的列

**final_rel->reltargetlist** 应该包含：
```
[j1.a, j2.a, j2.d, j3.a, j3.f]
```

注意：虽然 j2.a 和 j3.a 不在最终输出，但它们被 `add_vars_to_targetlist()` 添加了。

### 3. create_plan() 阶段

```c
// plan/createplan.c
Plan *plan = create_plan(root, best_path);
```

**create_plan() 的职责**：
- 输入：final_rel 的最佳 Path
- 输出：可执行的 Plan 树
- **关键**：根据 Path 的 `parent->reltargetlist` 构建每个节点的 targetlist

**生成的 Plan 树**（基于 PG EXPLAIN VERBOSE 的实际输出）：

```
Merge Join (顶层 j1⋈j2 ⋈ j3)
  Output: j1.a, j2.d, j3.f              ← 最终输出
  Merge Cond: j3.a = j1.a               ← 需要访问 j3.a 和 j1.a
  ├─ Sort
  │   Output: j3.f, j3.a                ← 包含 j3.a（JOIN 条件需要）
  │   └─ Seq Scan on j3
  │       Output: j3.f, j3.a
  └─ Materialize
      Output: j1.a, j2.d, j2.a          ← 包含 j2.a（虽然最终不输出，但子节点需要）
      └─ Merge Join (内层 j1⋈j2)
          Output: j1.a, j2.d, j2.a      ← 包含所有子节点的列
          Merge Cond: j1.a = j2.a
          ├─ Index Scan on j1
          │   Output: j1.a
          └─ Index Scan on j2
              Output: j2.a, j2.d, j2.e
```

**关键观察**：
- 内层 JOIN 的 Output 包含 `j2.a`，即使最终不需要
- 顶层 JOIN 从内层获取 `j1.a`（通过 Materialize 的 Output）
- 每个 JOIN 节点的 targetlist 都包含父节点需要的所有列

---

## Cascades 的错误执行流程

### 1. grouping_planner() 阶段

```c
// planner.c:1239-1240
upper_info.tlist = tlist;            // [j1.a, j2.d, j3.f]
upper_info.sub_tlist = sub_tlist;    // make_subplanTargetList() 的结果
```

**调试输出显示**：
```
CASCADES: upper->sub_tlist has 3 entries
```

**问题猜测**：`sub_tlist` 只包含 [j1.a, j2.d, j3.f]，缺少 JOIN 条件需要的中间列（j2.a, j3.a）。

### 2. pg_cascades_try_grouping_planner() 阶段

```c
// cascades.c:700
status = pg_cascades_try_grouping_planner(root, prep, &upper_info, &cascades_plan);
```

#### 2.1 调用 make_one_rel()

```c
// cascades.c:755-780
if (prep == NULL) {
    prep = pg_query_planner_prepare(root, upper->sub_tlist, ...);
}
```

**pg_query_planner_prepare() 内部**：
- 调用 `deconstruct_jointree()` → 应该调用 `add_vars_to_targetlist()`
- 调用 `make_one_rel()` → 生成 final_rel

**问题**：如果 `upper->sub_tlist` 不完整，`deconstruct_jointree()` 可能没有正确添加所有必要的列到 base relation 的 reltargetlist。

**推测的 base relation reltargetlist**：
```c
j1->reltargetlist = [j1.a]      // 正确
j2->reltargetlist = [j2.d]      // 错误！缺少 j2.a
j3->reltargetlist = [j3.f]      // 错误！缺少 j3.a
```

**推测的 final_rel->reltargetlist**：
```
[j1.a, j2.d, j3.f]  // 只有最终输出列，缺少中间 JOIN 列
```

### 3. create_plan() 阶段

```c
// planbuild.c:342
result = create_plan(ctx->root, (Path *) e->op_private);
```

**create_plan() 的行为**：
- 基于 `final_rel->reltargetlist` = [j1.a, j2.d, j3.f]
- 生成的 Plan 树的每个 JOIN 节点的 targetlist 也只包含这 3 列

**生成的错误 Plan 树**（推测）：

```
Merge Join (顶层)
  Output: j1.a, j2.d, j3.f          ← 正确
  Merge Cond: j3.a = j1.a           ← 需要 j3.a 和 j1.a
  ├─ Scan on j3
  │   Output: j3.f                  ← 错误！缺少 j3.a
  └─ Merge Join (内层)
      Output: j1.a, j2.d            ← 错误！缺少 j2.a
      Merge Cond: j1.a = j2.a
      ├─ Scan on j1
      │   Output: j1.a
      └─ Scan on j2
          Output: j2.d              ← 错误！缺少 j2.a
```

### 4. set_plan_references() 阶段

```c
// setrefs.c
plan = set_plan_references(root, plan);
```

**set_plan_references() 的职责**：
- 遍历 Plan 树
- 将每个表达式中的 Var 替换为对子节点 targetlist 的引用（OUTER_VAR/INNER_VAR）

**顶层 JOIN 的处理**：
- Merge Cond: `j3.a = j1.a`
- 需要在子节点的 targetlist 中找到 j3.a 和 j1.a
- **j3.a**：在左子树（j3 scan）的 targetlist 中查找 → **找不到**（只有 j3.f）
- **错误**：`variable not found in subplan target list`

---

## 为什么 2-Way JOIN 能成功？

```sql
SELECT j1.a, j2.d FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a;
```

**推测**：
- `sub_tlist` = [j1.a, j2.d]
- JOIN 条件需要 j1.a 和 j2.a
- **j1.a** 已经在 sub_tlist 中 ✓
- **j2.a** 不在 sub_tlist 中，但可能有以下情况之一：
  1. `make_subplanTargetList()` 智能地添加了 j2.a（因为它是 JOIN 条件）
  2. `deconstruct_jointree()` 正确调用了 `add_vars_to_targetlist()`，将 j2.a 添加到 j2->reltargetlist
  3. 2-way JOIN 比较简单，PG 有特殊处理

**需要验证的假设**：检查 2-way JOIN 的 final_rel->reltargetlist 是否包含 j2.a。

---

## 根本原因总结

**最可能的根本原因**：

1. **sub_tlist 不完整**：`make_subplanTargetList()` 只添加了最终输出列，没有添加 JOIN 条件需要的中间列。

2. **deconstruct_jointree() 未正确执行**：Cascades 调用 `pg_query_planner_prepare()` 时，`deconstruct_jointree()` 可能没有正确分析 3-way JOIN 的结构，导致中间 JOIN 列未被添加到 base relation 的 reltargetlist。

3. **final_rel->reltargetlist 不完整**：由于 base relation 的 reltargetlist 不完整，`make_one_rel()` 生成的 `final_rel->reltargetlist` 也不完整。

4. **create_plan() 忠实反映了错误**：`create_plan()` 基于不完整的 reltargetlist 生成 Plan，导致中间节点的 targetlist 缺少必要的列。

---

## 正规修复方案

### 方案 1：修复 sub_tlist（推荐）

**原理**：在调用 `make_one_rel()` 之前，确保 `sub_tlist` 包含所有 JOIN 条件需要的列。

**实现位置**：`cascades.c` 或 `planner.c`

**步骤**：

1. 在 `pg_cascades_try_grouping_planner()` 中，调用 `make_one_rel()` 之前：
   
   ```c
   // 扩展 sub_tlist 以包含所有 JOIN 条件列
   List *extended_sub_tlist = pg_cascades_extend_sub_tlist(root, upper->sub_tlist);
   prep = pg_query_planner_prepare(root, extended_sub_tlist, ...);
   ```

2. 实现 `pg_cascades_extend_sub_tlist()`：
   
   ```c
   List *
   pg_cascades_extend_sub_tlist(PlannerInfo *root, List *sub_tlist)
   {
       List *extended_tlist = list_copy(sub_tlist);
       ListCell *lc;
       
       // 遍历所有 JOIN 条件
       foreach(lc, root->parse->jointree->fromlist)
       {
           // 提取 JOIN 条件中的所有 Var
           List *join_vars = pull_vars_of_level((Node *) lfirst(lc), 0);
           ListCell *vc;
           
           foreach(vc, join_vars)
           {
               Var *var = (Var *) lfirst(vc);
               
               // 检查 var 是否已经在 extended_tlist 中
               if (!tlist_member(var, extended_tlist))
               {
                   // 添加到 extended_tlist
                   TargetEntry *tle = makeTargetEntry((Expr *) copyObject(var),
                                                       list_length(extended_tlist) + 1,
                                                       NULL,
                                                       true);  // resjunk = true
                   extended_tlist = lappend(extended_tlist, tle);
               }
           }
       }
       
       return extended_tlist;
   }
   ```

**优点**：
- 在源头解决问题
- 不需要修改 PG 核心代码
- 对所有 N-way JOIN 都有效

**缺点**：
- 需要正确解析 JOIN 树结构
- 可能引入额外的列（性能影响小）

---

### 方案 2：修复 final_rel->reltargetlist

**原理**：在 `make_one_rel()` 返回后，`create_plan()` 之前，扩展 `final_rel->reltargetlist`。

**实现位置**：`cascades.c`

**步骤**：

```c
// make_one_rel() 之后
RelOptInfo *final_rel = prep->final_rel;

// 扩展 final_rel->reltargetlist
List *join_vars = pull_vars_of_level((Node *) root->parse->jointree, 0);
ListCell *lc;

foreach(lc, join_vars)
{
    Var *var = (Var *) lfirst(lc);
    if (!tlist_member((Expr *) var, final_rel->reltargetlist))
    {
        final_rel->reltargetlist = lappend(final_rel->reltargetlist, copyObject(var));
    }
}
```

**优点**：
- 直接修复 final_rel
- 实现简单

**缺点**：
- 可能与 PG 的内部假设冲突
- 不如方案 1 优雅

---

### 方案 3：后处理 Plan 树（不推荐）

**原理**：在 `create_plan()` 之后，遍历 Plan 树，修复每个 JOIN 节点的 targetlist。

**问题**：
- 太晚了，`create_plan()` 已经基于错误的 reltargetlist 生成了 Plan
- 修改 Plan 树的 targetlist 可能导致其他不一致

---

### 方案 4：让 Cascades 自己构建 JOIN（长期方案）

**原理**：不使用 PG 的 IMPORTED_PATH，让 Cascades 递归构建 JOIN Plan。

**步骤**：
- 为 LogicalJoin 实现完整的 COMPOSABLE_OP 物理实现
- 递归构建左右子树的 Plan
- 手动构造 JOIN 节点，确保 targetlist 正确

**优点**：
- 完全控制 Plan 生成
- 符合 Cascades 架构

**缺点**：
- 实现复杂
- 需要大量工作

---

## 推荐的修复步骤

1. **立即修复（方案 1）**：实现 `pg_cascades_extend_sub_tlist()`，确保 sub_tlist 包含所有 JOIN 条件列。

2. **验证修复**：运行 coverage_80pct.sql，确保 SS5_3order 从 FALLBACK 变为 CASCADES。

3. **长期优化（方案 4）**：实现 Cascades 自己的 JOIN 构建逻辑，摆脱对 IMPORTED_PATH 的依赖。

---

## 附录：需要验证的假设

1. **sub_tlist 的实际内容**：在 `pg_cascades_try_grouping_planner()` 开头添加调试输出，打印 `upper->sub_tlist` 的每个条目。

2. **final_rel->reltargetlist 的实际内容**：在 `make_one_rel()` 返回后，打印 `final_rel->reltargetlist`。

3. **2-way JOIN 为什么成功**：对比 2-way JOIN 和 3-way JOIN 的 sub_tlist 和 final_rel->reltargetlist。

4. **deconstruct_jointree() 是否正确执行**：检查 `pg_query_planner_prepare()` 的实现，确认它正确调用了 `deconstruct_jointree()`。
