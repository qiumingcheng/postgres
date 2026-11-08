# 图 B-tree 与关系算子组合执行设计

2026-10-23第二轮Top 3已完成：smallint/integer键及跨整数连接、列级SELECT授权、同边表不同图元素映射。清单见第19节，实际验收见[报告第15节](../报告/图Btree组合执行M0-M6验收报告.md#15-第二轮-top-3-接入2026-10-23)。当前003604配套O2，PG共10tracked修改＋5新文件未提交，含前轮增量；图21/PG245/AGE43、新专项30通过。RLS和多标签UNION等边界未放宽，无新性能结论。下方记录为历史版本。

2026-10-22 Top 3 接入已完成（本轮尚未提交）：异构点/边表路径、bigint 单列键的列位置/唯一索引适配、复用顶点变量的闭环及自环。实现清单见第18节，实际验收见[报告第14节](../报告/图Btree组合执行M0-M6验收报告.md#14-pgq-top-3-接入补齐2026-10-22)。当前安装为101239配套O2；以下162702/工作树干净是本轮实施前状态，不得覆盖新代码。

2026-10-22提交状态：本轮代码已新增C/B/A增量提交`8937fa8`、`4bcae38`、`b9108dc`，PG工作树干净，旧三提交未改写，未push。源码与162702已测试版本完全一致，本次没有新增算法或测试结果；AUTO准确性和DML零退化仍未签收。下文未提交描述为历史，当前映射见[分层说明](../执行包/代码分层与独立迁移说明.md)。

## 1 目标与实施范围

2026-10-22最新限定续做见第17节及[报告第10节](../报告/图Btree组合执行M0-M6验收报告.md#10-delete差额诊断与无hint解析优化)：无hint解析优化已实现，只有小幅规划收益，DELETE纯执行差额和整体DML严格零退化仍未解决。三个无重复场景已在第16节取得有效计时；自然选路全面准确仍未验收。下面M0～M6勾选保留为首轮限定交付记录，不扩展G0～G6。

后续以**公共图执行层＋普通 B-tree provider**为主线。目标不是禁止关系执行，而是让适合的图区域使用专用多跳执行器，外围业务 JOIN、过滤、聚合、排序等继续使用 PG 算子。不需要 graph_adj 或 AGE 才能使用这条路线。

本设计基于当前工作树 `postgres19@5d3d3c1d29af20946b238a4bbde2c38bebeefd77`，不是根据 Neo4j 的能力推测本地实现。开发位置保持 `.worktrees/pg19-graph-core/postgres19`；原 PG19＋ORCA＋Daft、旧 AGE 实例不变。

2026-10-21：M0～M6本轮实施、回归和限定性能评估已结束，详见[阶段验收报告](../报告/图Btree组合执行M0-M6验收报告.md)。功能与兼容通过，性能结论有保留：新增矩阵5/7场景达到三干净轮，历史四场景复测均未达环境门，CBO准确性与DML零退化未签收。graph_adj、AGE不扩展存储/缓存行为。正式任务 `G1-080` 仍为 `in_progress`，以下M编号不是已通过的产品阶段门。

## 2 源码确认的现状

下表记录实施前的源码依据。路径相对 `.worktrees/pg19-graph-core/postgres19/`，函数名是定位依据；本轮具体改变与验收见第 7 节。

| 位置 | 现有行为 | 对本次设计的影响 |
|---|---|---|
| `src/backend/rewrite/rewriteGraphTable.c`，`generate_query_for_graph_path`、`build_pgq_fixed_program` | 同时生成原生关系子查询和可选 `GgcFixedProgram`；不能证明映射时保留关系查询 | 不能用“有 graph_program”作为关系路线的必要条件 |
| `src/backend/optimizer/prep/prepjointree.c`，`merge_pulled_graph_program` | 拉平子查询时转移图 IR；多个图 IR 合并会禁用 | 不能直接删除冲突保护来支持多个图区域 |
| `src/test/modules/test_graph_core/test_graph_fixed.c`，`route_graph_join_search` | 强制图路线要求整个 JOIN 区域恰好等于图变量集合，只构建线性元数据，不做该区域的 DP/GEQO | 保留纯图快速路径；外部 JOIN 必须另有正常规划入口 |
| 同文件，`pgq_program_contract` | 当前前端要求固定 OUT、同源点边表、非空 bigint 键、受限输出，拒绝多数边和中间点过滤 | 组合能力不等于这些语义已全部打通 |
| 同文件，`pgq_maybe_enable_endpoint_set`、`graph_upper_paths` | 仅安全的顶层终点 DISTINCT 可启用逐层去重；消除最终 DISTINCT 还要证明单起点唯一 | 混合查询首版保持完整 bag，不能沿用单起点结论丢重复路径 |
| `src/test/modules/test_graph_core/graph_route_hint.inc`，`route_planner` | 语句级 hint 配置一个图程序和一个索引；原来连 relational 也要求一个受支持图程序 | 先解耦 relational；多区域仍需新的逐区域配置 |
| `src/backend/optimizer/path/graphpath.c`，`GgcCreateGraphPath` | 校验完整 relids、quals、参数化边界；产物是普通 CustomPath，anchor 是其 child | 用正常 SubqueryScan/Path 接口组合，不伪造成本或路径顺序 |
| `src/backend/optimizer/path/allpaths.c`，`set_subquery_pathlist` | 为保留下来的子查询创建子规划器，可能下推过滤，之后生成 SubqueryScan | 保留图边界时也必须控制过滤下推，否则资格又被外围条件改变 |
| `src/backend/executor/graph_fixed.c`、`graph_btree_provider.c` | 共享多跳、批访问、可见性检查；B-tree provider 已有 OUT/IN 基础接口 | 后续扩展先补前端/计划映射，不能把底层 IN 接口当成用户 SQL 已支持 |

上述是实施前状态，当时标准入口仍需 `LOAD 'test_graph_core'`。M6 已迁入内核，现无需 LOAD；入口和证据见第14节。迁移模块代码与新增功能分开计量。

## 3 首批组合方案

### 3.1 SQL 与执行边界

示例使用现有固定 OUT 子集；`business` 是普通业务表，不是图元素表。

```sql
SELECT /*+ GraphRoute(btree) */
       t.category, count(*) AS matches
FROM GRAPH_TABLE (
    g MATCH (a IS v WHERE a.id = 1::bigint)
            -[x IS e]->(b IS v)-[y IS e]->(c IS v)
    COLUMNS (x.id AS first_edge, y.id AS last_edge, c.id AS endpoint)
) AS gt
JOIN business AS t ON t.vertex_id = gt.endpoint
WHERE t.enabled AND gt.last_edge > 0
GROUP BY t.category
ORDER BY t.category;
```

预期结构，不指定 PG 必须选哪一种 JOIN：

```text
Sort / Aggregate                         # 普通 PG 算子
└─ Hash Join / Merge Join / Nested Loop   # 外围 JOIN，由 PG 选择
   ├─ SubqueryScan                       # 必要时保留，含外围图列过滤
   │  └─ Custom Scan (GraphExpandFixed)   # 仅图区域，完整路径多重性
   │     └─ 点表 anchor 访问              # 普通 PG Scan
   └─ business 访问                      # 普通 PG Scan
```

`SubqueryScan` 若只做无用投影，PG 可以消除其显示节点；验收以真正出现 `GraphExpandFixed`、外围 JOIN 和正确结果为准，不强求计划打印一层空壳。边读取仍通过普通 B-tree，没有引入新索引 AM。

### 3.2 图区域如何留下来

仅在**显式 `GraphRoute(btree)`、一个受支持图程序、存在外围多输入查询层**时，启用语句内的图子查询边界策略。

1. 原生 PGQ 重写不变，仍保留关系表达作为独立对照。
2. 在公共优化器增加默认空的图子查询边界回调。只对有有效 `graph_program` 的子查询询问回调。
3. 策略开启时，`is_simple_subquery` 不把该图子查询拉平到业务 JOIN 中；不是给 SQL 偷加 `OFFSET 0`，也不滥用 `security_barrier`。
4. `set_subquery_pathlist` 对同一边界不做外围过滤下推。过滤留在 PG 的 SubqueryScan 或更上层，照常执行一次；并非丢掉过滤。
5. 子规划器进入完整图区域，复用已有快速生成 Path/Plan 的逻辑。外围没有图程序的查询层使用正常 DP/GEQO，JOIN 方法由原 GUC 和代价决定。
6. 外围不得触发测试专用的“普通 SQL 链识别”捷径。强制图 hint 只接受来自 PGQ provenance 的图区域。
7. `PG_FINALLY` 恢复边界回调、join-search hook 和语句配置，正常完成与 ERROR 都不能污染下一条语句。

纯图查询不启用新边界策略，继续使用原来的拉平＋独立图规划，保留已验证的终点集合优化。默认无 hint 与 `GraphRoute(adj)` 不因本次工作自动改变选路。

### 3.3 关系路线独立

`GraphRoute(relational)` 表示只采用普通关系执行，不要求能生成图 IR，允许原生 PG 已支持而图执行器尚不支持的方向、属性和复合键等查询。它也可用于普通只读 SELECT。

它不强制 Nested Loop，不修改 join/GEQO/Memoize 的开关；仍排除普通 graph_adj 索引访问和图 CustomScan。完整遍历 Append/MergeAppend/递归及 SubPlan 等已有计划容器，避免因为一个外围算子就误拒绝普通关系计划。

强制 btree 仍要求实际选择一个合格图节点；资格失败、没有合格 B-tree 或被简化到没有图节点时明确报错。无 hint 的原生回退保持可用，执行开始后不得透明重跑关系计划。

## 4 语义与安全审查

| 风险 | 首批处理 | 必须验证 |
|---|---|---|
| 平行边、同终点的不同路径 | 图子计划保持 bag；共享读取不合并输出次数 | 完整行的 multiset；不能只比较 count 或 set |
| 多起点全局 DISTINCT | 混合查询不启用顶层终点集合捷径；最终去重由 PG 完成 | 多起点汇聚、COUNT、DISTINCT 分开对照 |
| JOIN 放大或消除重复 | 业务 JOIN 完全由 PG 处理 | 一对多业务行、无匹配行、NULL 键 |
| LEFT/RIGHT/FULL JOIN 补 NULL | 保留图边界，让 PG 负责补 NULL；图内部仍是 inner fixed WALK | 两边各自无匹配行、ON 与 WHERE 条件区别 |
| 外围过滤进入图内部 | 首批禁止对被保留图区域做外围 qual 下推 | 图输出过滤、过滤全部数据；计划中真实存在过滤 |
| 外部 RLS 和权限 | 普通 PG 表继续走原路径；图内部仍按现有限制拒绝 RLS | 非超级用户、普通表 RLS、图表无权限或 RLS 被拒绝 |
| LATERAL/历史相关条件 | 不放宽 `required_outer` 与现有资格门 | 受限相关图查询必须报错，relational 正确执行 |
| 任意表达式、volatile、错误时序 | 首批只组合外围 PG 表达式；不把表达式搬到逐层去重前后 | 外围表达式、类型和 NULL；不承诺未验收的 volatile/异常时序 |
| 重扫和 LIMIT | 使用现有 CustomScan rescan/end，外部 Materialize/NestLoop 仍按 PG 契约 | 重复执行、游标、提前结束、错误后的下一条查询 |
| 参数与 DDL 失效 | 不在缓存 Plan 中保存回调、Relation 或临时索引指针 | PREPARE 的 custom/generic plan，索引重建/删除 |
| 统计误差 | 图内部用现有成本/PG 统计兜底；外围用 PG 行数传播 | 不把缺失统计写成零，不靠改低成本“实现”强制 hint |
| 大结果与内存 | 不在边界新增整图缓存/结果集合；原执行器预算不变 | 超限仍明确 ERROR，不能截断输出或切换语义 |
| 其他 provider/项目回归 | 公共回调默认 NULL，接口无 provider 依赖；AGE 默认 off | 现有图模块及原生 PG/AGE 回归；原环境路径和源码不变 |

静态审查结论：保留子查询是 PG 已有的计划组合方式，首批无需改执行器物理行身份、索引、磁盘/WAL或邻接 ABI；风险集中在规划边界与谓词归属。不能在实现前承诺“100% 无问题”，必须用下述正负例验证。边界内外的 JOIN 重排受限可能导致次优执行，这是首批方案的已知代价，不宣称所有混合查询更快。

## 5 实施清单与停止条件

本次范围固定为 M0～M6，不扩展为整个项目的 G0～G6，也不追加 graph_adj/AGE 性能优化。用户中途询问不取消实施；全部条目达到退出条件后交付最终结论并停止。勾选表示对应代码和所列验收均完成，不能以“已设计”或“已编码”代替通过。

- [x] **M0 关系路线解耦。** relational 不依赖图 IR，原生支持而图执行器不支持的查询可正常走关系计划。
- [x] **M1 单图区域组合。** 支持外围 PG JOIN、过滤、聚合、排序和 LIMIT；受限组合的正确性与兼容回归通过，证据见第 7 节。大规模性能评价归 M6，不随本勾选视为已通过。
- [x] **M2 属性与过滤扩展。** 已完成本设计准入范围内的实现与验证，见第 9～10 节。
  - [x] 点、边局部过滤的安全求值和唯一归属。实现与验收见第 9 节。
  - [x] typed 属性输出、表达式及跨变量/历史相关过滤。
  - [x] 重复路径、NULL、权限、volatile/不支持语义的准入与回退验证；有条件时不误用终点集合优化。
- [x] **M3 方向扩展。** IN/OUT 混合、无向匹配及对应索引选择；自环、平行边、方向输出与原生 PGQ 对照通过，见第 11.1 节。
- [x] **M4 多区域与自然选优。** 每图区域独立配置和候选；支持关系残余组合；无 hint 时按成本比较图/关系候选，强制 hint 逐区域检查。功能与成本竞争验证见第12.1节；代价预测精度/性能矩阵仍归M6。
- [x] **M5 有界图计划枚举。** 比较两端及合适的中间起点、展开顺序和已绑定键的闭合检查；候选/阶段工作量/软时间预算明确，功能验收见第13.1节。规划/执行收益归M6同期测量，不把单次计时当性能结论。
- [x] **M6 正式入口与收尾评估。** 已完成下列工作；性能不足/退化是交付结论，不冒充性能门通过。
  - [x] 稳定的规划前端和 CustomScan 包装移出测试模块，提供正式开关、EXPLAIN 和兼容入口。
  - [x] 同 SQL 正确性、权限/MVCC、重扫/取消/DDL、预算等生命周期回归，PG/AGE 兼容回归。
  - [x] 同 SQL 的规划/执行性能与选择率矩阵、DML 影响检查；环境不足及退化如实报告，不以追逐倍率延长任务。
  - [x] C、C+B、C+A、C+B+A 独立迁移矩阵，审阅并处理设计与执行包基线差异。
  - [x] 更新实现状态、测试报告和交接入口，列明通过项及仍不支持的边界，给出最终结论。

上述是本次工作清单，不代替正式 `task_state.json`；本轮完成也不自动表示其他 G 阶段或生产准入全部通过。

| 任务 | 交付内容 | 退出条件 |
|---|---|---|
| M0 关系路线解耦 | relational 不依赖图 IR；完整检查计划容器 | 原生支持但图路线不支持的 SQL 可正确走 relational；原 hint 词法/错误/GUC 回归通过 |
| M1 单图区域组合 | 本文第 3 节的边界策略、外围 PG JOIN/过滤/聚合/排序、明确计划证据 | 同 SQL btree/relational 完整 bag 相同，独立普通 SQL oracle 也相同；纯图、adj/AGE 回归不破坏 |
| M2 表达式与谓词归属 | 将局部边/点过滤、跨变量过滤及输出分开映射；绑定可见之后再求值 | NULL/权限/volatile/路径历史分类；无法安全共享的条件不启用逐层终点去重 |
| M3 方向扩展 | OUT/IN 混合阶段映射、双向 B-tree 选择；无向扩展对自环仅产生一次原生匹配 | 每方向、反向边、平行边、自环、环与原生 PGQ 完整 bag 一致 |
| M4 多区域与自然选优 | 每区域有独立程序和访问候选；允许图子区域与关系残余组合，替代全局单个 cbo_config | 不能靠删除多程序冲突保护交付；强制 hint 逐区校验；无 hint 候选有公平成本比较 |
| M5 有界图计划枚举 | 两端及选择性较高的中间点作为候选起点；阶段连接和 ExpandInto 的资格/代价 | 固定候选预算，规划耗时、执行时间分开；不能退回点边级全排列而丢失规划收益 |
| M6 产品入口与阶段验收 | 将已稳定的前端/CustomScan 包装移出测试模块，配套 GUC、EXPLAIN、升级/回归与四组合检查 | 实现迁移不计作新增算法；完整 G1 及后续门按正式任务账本验收 |

M2 的内部设计：每个条件保存依赖变量集合和安全等级。只依赖当前已可见点/边的安全条件可下推；跨历史的条件在绑定完整后执行；外围条件继续由 PG 消费。每个 qual 必须恰好有一个语义责任方，不允许只检查“字段存在”。输出由 typed slot 映射，逐步替代现有固定一个 payload 列的限制。

M3 的内部设计：provider 方向和 SQL 逻辑边方向分开；反向遍历不能把 `src`/`dst` 的用户输出颠倒。无向同一物理边的两方向匹配在非自环时保留各自绑定，自环只保留原生 OR 语义的一次匹配；不同物理边即使端点相同也不能合并。

M4/M5 需要单独审查 `Query.graph_program` 到区域集合的演进、copy/equal/out/read/jumble、RTI 重映射、ParamPathInfo 与缓存失效。首批保留子查询是可运行基础，不冒充已经完成这些全局优化。

## 6 测试与验收方法

先固定源码和构建身份，使用本项目 `pgq-integrated` O2 配套 PG/模块/AGE。新建自有隔离测试实例，不启动或重建用户种子库。

1. **先有失败用例。** 在旧二进制上确认 mixed btree 被拒绝、原生 relational 能给出预期答案；保存基线，不仅检查字符串。
2. **原生同 SQL 对照。** 同一 `GRAPH_TABLE` SQL 仅换 hint，先 EXPLAIN 确认真有目标节点，再执行比较值、NULL、类型与多重性。
3. **独立 oracle。** 小图额外用显式点表/边表 JOIN 验证，避免两个 hint 都走同一错误捷径。包含平行边、自环、缺失顶点和业务重复行。
4. **计划证据。** 保存完整 TEXT 计划，包括至少 Hash Join、Nested Loop、外连接、聚合、过滤和排序；外部 JOIN 方法可由测试 GUC 定向，但正常会话设置不得泄漏。
5. **边界与生命周期。** 单起点/多起点、空结果、普通表 RLS、图 RLS 拒绝、generic/custom prepared plan、重扫、取消/错误恢复分别验收。
6. **兼容门。** 重跑 13 项图模块、245 项原生 PG 与配套 AGE；任何失败保留原始日志，分析原因后重跑，不能调整 oracle 制造通过。
7. **性能单列。** 本轮首先交付功能组合，不承诺混合查询 10×。随后使用同 SQL、热态、相同数据/索引，分别测 planning/execution；原四类 10 万边结果不能直接算作新增组合功能成绩。

实现前现有图模块基线为 13/13 PASS，证据：
[20261021 图模块基线](../执行包/evidence/runs/20261021T082229.916487Z-G1-080-graphroute-recovery-modules/summary.json)。

执行包 `validate_package.py` 在本次编辑前已报主设计摘要漂移。正式基线重新绑定需要审阅主设计与契约差异，不能只更新 SHA 来使校验变绿。该问题不等于本轮功能测试失败，也不能用本轮定向测试宣布完整 G1 通过。

## 7 M0 M1 实施记录

M0/M1 已实现，PG HEAD 仍为 `5d3d3c1`，增量尚未提交；AGE 源码未改。本节保留首批实施与验证记录，M2 后续代码及最新证据见第 9.4 节。内核公共接口增加 `GgcGraphSubqueryBoundaryHook` 与 `GgcGraphKeepSubqueryBoundary`，对应 pullup/qual pushdown 两处调用；语句级 B-tree 策略、外围标准 JOIN 入口与计划校验在既有模块内实现。

实际改动没有新增多跳执行算法或索引：4 个内核 C/H 文件增加 28 行，前端模块 C/include 片段增加 41 行、删除 8 行，另有测试注册及 258 行新 SQL、1 行 expected。小增量只对应“利用已有子计划接口打通受限组合”，不能用它推断 M2～M6 也只需这些代码。

| 检查 | 结果 | 可核对证据 |
|---|---|---|
| 旧版本失败重现 | 同一两跳＋业务 JOIN，relational 正确，btree 返回 0A000 | [基线](../执行包/evidence/runs/20261021T083440.988924Z-G1-080-btree-composition-baseline/summary.json) |
| 配套 O2 构建 | PG、test_graph_core、AGE 全部重新构建/安装；源、库摘要匹配 | [构建](../执行包/evidence/runs/20261021T084007.768687Z-build-pgq-integrated/summary.json) |
| 图模块 | 14/14 PASS；包含原 13 项及新增 `graph_composition` | [模块回归](../执行包/evidence/runs/20261021T084052.908944Z-G1-080-graphroute-recovery-modules/summary.json) |
| 原生 PG | 245/245 PASS | [PG 回归](../执行包/evidence/runs/20261021T084126.622387Z-regress-pgq-integrated-pg/summary.json) |
| AGE | 43/43 PASS | [AGE 回归](../执行包/evidence/runs/20261021T084203.217068Z-regress-pgq-integrated-age/summary.json) |
| 独立组合与生命周期 | 8 类查询×2 路线与显式 JOIN oracle 一致；4 类生命周期检查通过 | [12 项专项](../执行包/evidence/runs/20261021T084353.943679Z-G1-080-btree-composition-accept/summary.json) |
| AGE 开关与既有缓存契约 | 默认 off、无 provider 拒绝、缓存计划切换及非 superuser 拒绝等 5 组通过 | [GUC 回归](../执行包/evidence/runs/20261021T084507.636855Z-G1-080-pgq-integrated-guc/summary.json) |

新增模块回归覆盖：完整 bag 与 NULL payload、业务表重复行、单/多起点、DISTINCT/COUNT、外围过滤、INNER/LEFT/RIGHT/FULL/CROSS JOIN、表达式、排序/LIMIT、包装子查询、三种 JOIN 实现、分区 Append、禁用 Materialize 后真实多次 Graph Rescan、业务表 RLS、图边 SELECT 权限、custom/generic prepared plan、REINDEX、资格失败后的 hook 恢复。relational 另测无向、IN、UNION ALL、递归及普通 SELECT。

专项生命周期实测：游标和 RR 事务保持旧快照的 6 行；另一会话更新边后，提交再查能看到新结果 4 行。还验证了取消后复用同 backend、删除/重建所需索引后的缓存计划失效和恢复、原生隐式 lateral 查询的关系执行及图路线拒绝。

完整 Normal 计划保存在专项目录，例如 [LEFT JOIN＋外围过滤](../执行包/evidence/runs/20261021T084353.943679Z-G1-080-btree-composition-accept/left-btree.txt)、[Hash Join](../执行包/evidence/runs/20261021T084353.943679Z-G1-080-btree-composition-accept/hash-btree.txt)、[Merge Join](../执行包/evidence/runs/20261021T084353.943679Z-G1-080-btree-composition-accept/merge-btree.txt)、[聚合](../执行包/evidence/runs/20261021T084353.943679Z-G1-080-btree-composition-accept/aggregate-btree.txt)。该目录同时保留对应 relational 计划和完全相同的 SQL 正文。

首次新增模块测试因 PL/pgSQL 局部变量与 CTE 列同名失败；首次生命周期测试误写了原生语法不接受的显式 `LATERAL GRAPH_TABLE`。均修正测试本身，失败日志保留，没有更改 expected 或放宽产品资格来制造通过。

复验入口位于 `执行包/golden/G1-080/`：

```bash
python3 pgq_integrated_build_v1.py
python3 pgq_integrated_accept_v1.py modules
python3 pgq_btree_composition_v1.py
python3 pgq_integrated_accept_v1.py pg
python3 pgq_integrated_accept_v1.py age
python3 pgq_integrated_accept_v1.py guc
```

`--baseline` 只用于匹配的旧二进制，不能在新版本预期旧失败。所有测试使用自有新建实例并已停止，旧种子库、ORCA/Daft、原 AGE 实例未动。既有大规模性能表没有重新测量，本轮不宣布性能倍率或 DML 非劣化。

## 8 评估结论与下一阶段前置条件

**首批受限方案的可行性与列明的正确性场景已经验证；完整路线尚未充分评估到可以一次性实施，更不是“100% 无问题”。** 当前准入仍是单个合格固定 OUT 图区域，显式 hint，不默认启用混合计划。

首批方案的主要权衡是边界限制重排。例如图内所有起点能产生一百万条路径，而业务表条件最终只需一个起点：当前可能先完成很大的图展开，再与小业务集合 JOIN；原生关系路线有机会先过滤业务表并通过连接条件缩小图访问。这是具体的退化机制，不是已经测得的某个倍率。自然选优前必须测选择率矩阵，并评估受安全限制的条件下推、参数化起点路径与外围 JOIN 成本，不能看到纯图规划快就默认强制 btree。

| 尚未关闭的问题 | 进入下一步前要补的设计或证据 |
|---|---|
| M2 条件/属性泛化 | typed binding/slot 的字段与寿命、qual 的唯一归属、跨历史与 volatile/error 行为；不能沿用单 payload 接口假装任意属性已支持 |
| M3 IN/无向 | 正反向输出映射、自环去重而平行边不去重、各阶段方向的索引候选与代价；需独立原生 bag oracle |
| M4 多区域 | Query 序列化、RTI/外部参数映射、区域间相关性、共享连接条件、逐区域配置，不能共享当前单个全局 cbo_config |
| M4/M5 自然选优 | 选择率/扇出/汇聚的留出矩阵；索引选择不能长期以最小 OID 代替代价；候选数量、时间与内存预算需明确 |
| 混合查询性能 | 同 SQL/同数据的 planning、first-row、execution 及大结果/小 LIMIT；不能用本次毫秒级小图验证替代十万边基准 |
| 产品与组合安全 | 新版本 C/C+B/C+A/C+B+A 四组合、复杂 RLS/安全视图、并行/worker、完整 SSI 与资源压力尚未作为本轮全门验收 |
| 工程基线 | 主设计与执行包摘要漂移须实质审阅后重绑定；当前未提交增量和测试模块入口须按既定分层处理 |

M2 已按第 9 节完成局部过滤这一步；下一步仍须细化属性输出与跨变量条件的绑定/生命周期，不能直接把后续任务表当成已经审查完成的详细实现规范。M1 的选择率/规模性能评价纳入 M6。

## 9 M2 局部过滤实施设计

第一步已接入只依赖单个图变量的边、目标点过滤；起点条件继续由 anchor 子计划执行。跨变量条件和任意属性输出属于 M2 后续部分，不能以此步完成代替整个 M2。以下为实现契约，验收记录见第 9.4 节。

### 9.1 条件的唯一归属

| 条件 | 执行位置 | 约束 |
|---|---|---|
| 起点属性条件 | 原 PG anchor Scan | 保留原始 baserestrictinfo，不在图阶段重复执行 |
| 点边连接键等式 | provider 定向查找＋目标点 MVCC 检查 | 必须完整匹配原 topology RestrictInfo |
| 首跳 src 常量且被起点等式蕴含 | 已有等价证明 | 继续保留原快路径，不为冗余条件关闭集合优化 |
| 单条边的局部条件 | MVCC 批回表后、创建路径绑定前 | 使用真实边元组，支持 NULL 和不同属性类型；不把索引候选当可见行 |
| 单个目标点的局部条件 | 点主键探测取得可见行后、创建路径绑定前 | 本阶段需要属性时必须回表；不能从 VM 可见位假造属性 |
| 跨变量、历史相关条件 | 本步仍拒绝强制图路线 | 后续提供完整 typed binding 后单独接入 |
| 图外条件 | 普通 PG 算子 | M1 的边界和过滤下推限制不变 |

只接入安全级别 0、无外部关系依赖、无 volatile/SubPlan/SRF/整行或系统列引用的局部表达式。允许普通标量函数和外部绑定参数；不保证 SQL 本就没有定义的不同计划间报错先后顺序。图内 RLS、相关子查询等仍走原生关系路线，强制 btree 明确拒绝。

### 9.2 字段和生命周期

公共执行层增加运行期 `GgcFixedLocalFilter` 和 `GgcFixedSetLocalFilters`：每阶段的边/点回调各有 context，只在 Begin 后、首次 Next 前注册；执行器复制回调数组，不把指针序列化到计划。边回调借用 `GgcBatchTupleView`，点回调借用 `TupleTableSlot`，寿命仅限回调内，不允许保留元组指针。

前端将每阶段两组条件保存到 `CustomScan.custom_exprs`，由 PG `setrefs`、参数处理、表达式依赖失效和 `ExecInitQual` 正常处理。`custom_scan_tlist` 包含输出 Var 及仅供过滤的隐藏 Var；固定列号/原属性号映射保存在整数 List 中。独立过滤 slot 只在回调内填充当前变量的属性，其余列置 NULL，随后 `ExecQual` 并清空；输出 slot 不与过滤共享借用状态。隐藏属性不冒充已支持的用户输出属性。

所有有局部过滤的本批计划保持完整 bag；暂不允许 factorized/endpoint-set/AGE 内存邻接优化，公共 setter 和优化启用接口双向防止错误组合。没有局部过滤的既有路径不受影响。点扫描对象仍可跨阶段复用；是否免回表按当前阶段是否有点属性条件判断，不能沿用扫描 owner 的第一个阶段决定后续阶段。

缓存 Plan 只含节点、OID 和列号；`ExprState`、slot、回调及 context 每次执行重建，rescan 保留只读表达式并重新读取当前参数，不缓存条件结果。额外需要点属性的接口保守要求点表 SELECT，不能仅凭 key 列权限读取任意属性。

### 9.3 估算与验收

不新增统计目录。局部条件选择率和求值成本读取原 PG 统计/表达式估价；首跳 `edge.rows` 仅在已证明是起点常量约束时可作为该起点扇出，不能把整表属性筛选后的行数当单点邻居数。扫描/顶点检查费用先保守计入未过滤候选，不以压低成本实现强制 hint。

验收覆盖：首跳/后续跳边过滤，中间点/终点过滤，NULL/JSONB/数值/文本属性、组合条件与参数；平行边、自环、缺失点保持原生完整 bag；DISTINCT 不误启用逐层去重；混合 JOIN、重扫、prepared custom/generic、函数/索引 DDL 失效、权限与取消后复用。相同 GRAPH_TABLE 只换 hint，另用显式普通 JOIN 作独立 oracle。原无过滤图模块、PG 和 AGE 回归不得退化。

### 9.4 局部过滤实现与验收

公共接口和实现位于 `src/include/executor/graph_fixed.h`、`src/backend/executor/graph_fixed.c`；前端的条件准入、序列化、过滤 slot 与 EXPLAIN 位于 `src/test/modules/test_graph_core/test_graph_fixed.c`。没有新增索引、统计目录或持久缓存，没有改变 adj V7/lease v4。新入口暂只对显式 B-tree 开放；adj 和原 no-hint 前端的资格不扩大。

| 检查 | 结果 | 证据 |
|---|---|---|
| PG、模块、AGE 配套 O2 重建 | PASS | [当前构建](../执行包/evidence/runs/20261021T091031.281264Z-build-pgq-integrated/summary.json) |
| 图模块 | 15/15 PASS；新 `graph_local_quals` 含 70 组主矩阵×2 路线，另测参数/重扫/DDL/拒绝及错误恢复 | [模块](../执行包/evidence/runs/20261021T091210.724915Z-G1-080-graphroute-recovery-modules/summary.json) |
| 独立局部过滤专项 | 8 类 PASS；同 SQL/显式 JOIN oracle、并发更新/RR、TOAST、普通用户、权限/RLS、取消恢复 | [专项](../执行包/evidence/runs/20261021T091231.929995Z-G1-080-btree-local-quals/summary.json) |
| 原 M1 组合与生命周期 | 12 类 PASS | [组合](../执行包/evidence/runs/20261021T091602.016683Z-G1-080-btree-composition-accept/summary.json) |
| 原生 PG / AGE | 245/245、43/43 PASS | [PG](../执行包/evidence/runs/20261021T091248.134834Z-regress-pgq-integrated-pg/summary.json)、[AGE](../执行包/evidence/runs/20261021T091455.941562Z-regress-pgq-integrated-age/summary.json) |
| AGE 默认开关和缓存契约 | 5 组 PASS | [GUC](../执行包/evidence/runs/20261021T091614.218863Z-G1-080-pgq-integrated-guc/summary.json) |

[完整 Normal 计划](../执行包/evidence/runs/20261021T091231.929995Z-G1-080-btree-local-quals/all-local-stages-btree.txt)显示 2 个图阶段、4 个实际局部条件、过滤移除 3 个候选绑定，最终输出 1 行。两阶段都需要点属性，因此实际回表读取；不能用 VM 省掉该读取。此为功能证据，不是新性能倍率。

首次模块运行保留于 `20261021T090933.158684Z-G1-080-graphroute-recovery-modules`：旧测试仍要求 B-tree 拒绝局部过滤，现改为断言过滤后的精确路径；adj 原拒绝不变。M1 的错误恢复用例改用仍不支持的 volatile 内部条件。新增跨变量负例由原生不支持的元素内引用，修正为合法的图模式级 WHERE。没有修改 expected 来吞掉错误。

当前 PG 为 10 项 tracked 修改＋4 个未跟踪 SQL/expected 文件；累计 tracked diff SHA256 为 `e9509c7da20b99cda2f48e706095f837cbe49839fea6c329e704681cad863ca7`，新文件摘要见构建清单。未 commit/amend，AGE 源码 clean。所有自有实例已停。复验新增入口为 `执行包/golden/G1-080/pgq_btree_local_quals_v1.py`，其他构建/回归入口仍见第 7 节。

M2 剩余属性输出与跨变量过滤仍未完成；M3～M6 未打钩。本轮不声称新性能、自然 CBO、DML 或四组合迁移已验收。

## 10 M2 属性输出与跨变量条件设计

本步继续使用完整路径绑定，不修改 provider、页格式或 AGE 缓存。新增属性按已绑定的唯一逻辑键、同一查询快照延迟读取；基础键和已有 payload 仍直接使用执行器结果。它是扩大语义覆盖的实现，不是新的性能优化。

### 10.1 类型与读取身份

`custom_scan_tlist` 只描述原表 Var；用户输出的运算、转换、JSONB 提取等保留在普通 `Plan.targetlist`，交给 `ExecProject`。不能把转换前的 Datum 填入转换后类型的列。

点的非 key 属性、边的新增属性按图变量各配置一个属性读取描述符：原表 OID、立即生效的单列 bigint 主键 B-tree OID、绑定种类与位置。运行期复用索引扫描对象，用 `GgcFixedResult.vertices/edges` 中的 key 定位同一快照下唯一可见行。准入须验证表级 SELECT、无 RLS、主键和普通本地表；新属性不满足这些条件时强制图 hint 拒绝，原生关系路线仍可用。缓存计划记录属性索引依赖，运行时再次校验。

同一变量的多个属性只读取一次。属性拷贝到当前输出行的 per-tuple context 后清空源 slot、释放扫描 pin；不缓存跨行或跨事务结果。重扫重新读数据和参数；结束关闭扫描和 Relation，错误/取消由 PG ResourceOwner 清理。扫描对象数量随需要属性的图变量数增长，不随路径结果数增长。

### 10.2 条件归属与多重性

拓扑等式仍由定向展开负责，局部条件仍在当前点/边可见后执行。其余安全、仅依赖本图区域变量的跨变量条件放到 `Plan.qual`，在完整绑定及必要属性齐全后执行 `ExecQual`，通过后才投影。每个原 RestrictInfo 必须出现在拓扑、局部或剩余条件的消费集合中；安全级别、外部依赖、volatile、SubPlan、SRF 等资格限制不能跳过。

带跨变量条件的查询不启用逐层终点集合或 factorized 捷径。同一终点来自不同历史路径时逐条求值，平行边及结果重复次数保留。隐藏的局部条件属性只在局部 slot 中存在，不为它们额外读取输出属性。

### 10.3 代价与验收门

每个需要延迟属性读取的图变量按完整绑定数增加一次主键定位/回表成本；剩余条件使用 PG 的表达式成本与选择率。暂不承诺对属性密集查询比 relational 更快；选择性低且后置过滤很多时可能明显多做工作，后续自然 CBO 必须能选择关系方案。

验收要求同一 GRAPH_TABLE 仅换 hint，并与独立显式 JOIN 比较完整行 bag、类型和 NULL；包含数值、文本、JSONB/TOAST、强制类型转换、历史条件、平行边、多起点、COUNT/DISTINCT、外围 JOIN、prepared 参数、主键/函数 DDL、权限、RR 并发更新和取消恢复。新增字段和表达式仍须保留在 PG 可追踪的计划节点中，不能隐藏 Expr 或运行期指针。

### 10.4 实现与验证

本步已按上述契约实现。另修正 anchor 子计划中 key 的列号：从实际 targetlist 定位，不因用户先输出 `a.name` 就误把第一列当 bigint key。属性读取在同一查询快照下逐变量复用扫描对象，表达式和类型转换由 PG 投影处理；没有改持久存储、统计目录或 AGE 源码。

| 检查 | 结果 | 证据 |
|---|---|---|
| PG、模块、AGE 配套 O2 构建 | PASS | [构建](../执行包/evidence/runs/20261021T093823.497322Z-build-pgq-integrated/summary.json) |
| 图模块 | 16/16 PASS；新增属性矩阵 30 组×2 路线，另测聚合/DISTINCT/混合 JOIN、参数、类型、NULL、TOAST、DDL、错误恢复 | [模块](../执行包/evidence/runs/20261021T093930.124653Z-G1-080-graphroute-recovery-modules/summary.json) |
| 属性与历史条件专项 | 12 类 PASS；同时检查结果类型、完整 bag、RR/游标并发更新、权限撤销、RLS、函数/索引失效、100 次早停及取消恢复 | [专项](../执行包/evidence/runs/20261021T094147.753541Z-G1-080-btree-properties/summary.json) |
| 原生 PG / AGE | 245/245、43/43 PASS | [PG](../执行包/evidence/runs/20261021T094231.100938Z-regress-pgq-integrated-pg/summary.json)、[AGE](../执行包/evidence/runs/20261021T094348.207903Z-regress-pgq-integrated-age/summary.json) |
| 局部过滤 / 外围组合 / GUC | 8 类、12 类、5 组 PASS | [局部过滤](../执行包/evidence/runs/20261021T094416.503336Z-G1-080-btree-local-quals/summary.json)、[组合](../执行包/evidence/runs/20261021T094417.578824Z-G1-080-btree-composition-accept/summary.json)、[GUC](../执行包/evidence/runs/20261021T094418.706251Z-G1-080-pgq-integrated-guc/summary.json) |

[完整 Normal 计划](../执行包/evidence/runs/20261021T094147.753541Z-G1-080-btree-properties/typed-path-history-btree.txt)显示：6 个完整绑定、1 个跨变量条件、过滤 3 行并返回 3 行；5 个属性扫描对象共读取 30 行。这个例子明确体现延迟属性读取成本，不能把它说成免回表优化或新性能结论。

本步 PG 累计 10 项 tracked 修改＋6 个新 SQL/expected 文件，tracked diff SHA256=`941ea5cd9e029c87046536d2ff2e38814dfaa18ec1640e5219ed203e5e24aa41`；新增文件摘要见构建。未提交，AGE clean，所有自有实例已停。新增复验入口 `执行包/golden/G1-080/pgq_btree_properties_v1.py`。M2 勾选不代表任意 SQL/任意数据源都支持：RLS、volatile 内部条件、相关子计划、复合键等仍保留关系执行，强制图 hint 明确拒绝。M3～M6 和正式阶段门不由本步替代。

## 11 M3 方向扩展实施设计

先接通已有执行器的 IN/OUT：每阶段根据查询方向选择普通 `src` 或 `dst` B-tree；逻辑边的 src/dst 输出必须映射真实边方向，不能把反向遍历当成改写边本身。首跳常量蕴含证明和 PG 统计兜底也按方向选择列，方向不同的图不能共用 OUT 的路径统计指纹。

无向阶段按同一快照读取 OUT 与 IN 两路。OUT 正常交付，IN 只排除 `src=dst=当前点` 的自环重复；不同 id 的平行边以及从不同起点产生的绑定全部保留。公共执行器以有界批次顺序消费两路，不生成 2 的跳数次方个计划。反向访问独立验证 provider/index/snapshot 权限和重扫 scope，输出和异常清理仍遵守现有生命周期。

图 IR 记录无向语义，provider 请求仍只含 OUT 或 IN，不能要求旧 provider 接受未定义的新方向。首版无向保持 bag，不启用已有单向 factorized/endpoint-set/AGE 快路径；无向查询需要实际 src/dst 属性时按 M2 的唯一边身份取值。准入精确验证原生 `(src=a AND dst=b) OR (dst=a AND src=b)`，不能只看到 OR 就认为是拓扑。

验收按 1～3 跳的方向组合，加入反向、平行、自环、环、缺失顶点、多起点、属性/历史条件和外围 JOIN，逐条与原生 PGQ/显式 JOIN bag 比较；两索引缺失、DDL、重扫及原 OUT 功能快路径另查。性能归 M6，不因方向功能通过宣称性能提升。

### 11.1 M3 实现与验收

IR 新增逻辑 `GGC_UNDIRECTED`，支持原生无向模式的 lowering、pullup 映射验证；provider 仍只接受原 OUT/IN 请求。`GgcFixedSetUndirectedStages` 在首次 Next 前注册反向访问；每阶段依次消费两路并跳过反向自环副本，重扫验证两套 scope。没有新增 2 的跳数次方个计划，也没有改变 provider ABI/索引页格式。

| 检查 | 结果 | 证据 |
|---|---|---|
| 配套 O2 构建 | PASS | [构建](../执行包/evidence/runs/20261021T095458.767988Z-build-pgq-integrated/summary.json) |
| 图模块 | 17/17 PASS；1～3 跳全部 39 种方向组合×2 起点方式×4 查询形态×2 路线，另测过滤/自环/索引失效 | [模块](../执行包/evidence/runs/20261021T095616.201012Z-G1-080-graphroute-recovery-modules/summary.json) |
| 方向生命周期专项 | 8 类 PASS；混合历史条件、真实重扫、RR/游标并发更新、仅 IN 索引、参数/REINDEX、九跳无向、取消恢复 | [专项](../执行包/evidence/runs/20261021T095620.671742Z-G1-080-btree-directions/summary.json) |
| PG / AGE | 245/245、43/43 PASS | [PG](../执行包/evidence/runs/20261021T095659.255913Z-regress-pgq-integrated-pg/summary.json)、[AGE](../执行包/evidence/runs/20261021T095719.322163Z-regress-pgq-integrated-age/summary.json) |
| 属性 / 外围组合 / GUC | 12 类、12 类、5 组 PASS | [属性](../执行包/evidence/runs/20261021T095725.787680Z-G1-080-btree-properties/summary.json)、[组合](../执行包/evidence/runs/20261021T095726.972765Z-G1-080-btree-composition-accept/summary.json)、[GUC](../执行包/evidence/runs/20261021T095728.117927Z-G1-080-pgq-integrated-guc/summary.json) |

[九跳完整 Normal 计划](../执行包/evidence/runs/20261021T095620.671742Z-G1-080-btree-directions/nine-undirected-btree.txt)为一个 9-stage 图节点，在 10 点直线上返回 126 个 WALK 匹配；与同 SQL relational 和显式 JOIN 一致。[重扫计划](../执行包/evidence/runs/20261021T095620.671742Z-G1-080-btree-directions/outer-rescans-btree.txt)为 UNDIRECTED、IN 两阶段，外侧 3 行导致图节点实际执行 3 次，9 次 scope 重验包含反向访问。

首次整套模块失败仅因原 provenance 用例要求无向没有 IR。该用例现改为明确检查重写前后各一个程序、合法原表映射及未禁用；期望布尔结果仍为 true。原失败日志保存在 `20261021T095400.520325Z-G1-080-graphroute-recovery-modules`，未用修改结果吞错。

M3 验收点累计 14 项 tracked 修改＋8 个新测试文件，tracked diff SHA256=`73ee2e123d8ef6576521431a6e8818fb7d90eb0f0f7195fa7bc5003bdef2a5e2`；未提交，AGE clean，自有实例停止。下一步 M4；M3 功能通过不替代自然选优/性能/生产门。

## 12 M4 多区域与自然选优设计

继续使用 Query 树中各自独立的 `graph_program`，保留图子查询边界，不把多个程序硬塞进一个全局程序，也不删除冲突保护。图区域的配置按 PlannerInfo 分别保存；构建候选时临时使用本区域配置并在正常/错误返回时恢复，最终 Path/Plan 持有自己的 OID、阶段和输出映射。外围业务 JOIN、多个图区域之间的 JOIN 和关系残余仍由 PG 规划。

强制 B-tree hint 要求每个图区域实际选中合格图节点；不能只检查第一个索引、不能让一个区域使用另一区域的表或 stage_count。unsupported 区域不能在强制模式静默绕过。adj 强制资格维持原单区域范围。

新增默认 off 的 `enable_graph_btree`，开启后无 hint 查询可按区域增加 B-tree 候选；不支持的形态、缺索引、成本较差时保留关系路径，不能通过捕获任意执行错误再换计划。此阶段自然选优保留标准 JOIN 搜索以获得真实关系候选；不能把强制 hint 的规划收缩倍率宣称为无 hint 已取得的收益。候选成本包括 M2 属性读取、局部/历史过滤和顶点可见性，禁止改低成本强迫胜出。

默认关闭时原无 hint 和测试配置不变；显式 relational 总是只比较普通关系计划，显式图 hint 可覆盖自然开关，但不能覆盖语义与权限准入。多区域自然模式保留边界的跨区域重排限制须在 M6 选择率矩阵明确评估；暂不支持参数化图区域的 lateral 依赖。

验证两套不同点边表/跳数/索引的区域，INNER/LEFT/聚合/UNION 容器，区域内条件与外围残余，同 SQL 原生 bag、独立 JOIN、重扫和缓存计划失效；自然模式必须分别观测图胜出、关系胜出、缺索引/不支持语义正常关系回退。实现验收如下。

### 12.1 M4 实现与验收

每个 `PlannerInfo` 对应一个 `RouteRegionConfig`，独立记录图/边/索引/角色/统计scope；构造候选时临时安装旧模块配置，`PG_FINALLY` 恢复。保留每个合格图子查询，外围仍由普通 PG 搜索；没有删除多程序冲突保护。强制B-tree要求所有原始区域通过，逐阶段检查访问族；adj仍单区域。默认off的正式GUC `enable_graph_btree` 控制无hint自然候选，仍需当前测试模块入口（M6迁移），不修改JOIN/GEQO开关或成本系数。它是规划期开关，已缓存计划的重规划遵循PG规则，修改后需重新PREPARE或DISCARD PLANS。

| 检查 | 结果 | 证据 |
|---|---|---|
| PG/模块/AGE 配套O2重建 | PASS | [构建](../执行包/evidence/runs/20261021T101453.528301Z-build-pgq-integrated/summary.json) |
| 图模块 | 18/18 PASS | [模块](../执行包/evidence/runs/20261021T101514.785386Z-G1-080-graphroute-recovery-modules/summary.json) |
| 多区域/自然选优/生命周期专项 | 11类 PASS | [专项](../执行包/evidence/runs/20261021T101519.454122Z-G1-080-btree-regions/summary.json) |
| PG / AGE 原生回归 | 245 / 43 PASS | [PG日志](../执行包/evidence/runs/20261021T101626.948324Z-regress-pgq-integrated-pg/regression.log)、[AGE日志](../执行包/evidence/runs/20261021T101646.709381Z-regress-pgq-integrated-age/regression.log) |
| M3方向 / AGE开关 | 8类 / 5组 PASS | [方向](../执行包/evidence/runs/20261021T101653.430319Z-G1-080-btree-directions/summary.json)、[GUC](../执行包/evidence/runs/20261021T101654.587627Z-G1-080-pgq-integrated-guc/summary.json) |

两区域的不同表、1/2跳、OUT/无向、属性与外围条件已比较完整bag；包括LEFT/FULL/UNION、实际重扫、参数计划、第二区域权限撤销、三索引REINDEX、RR游标期间另一会话写入。缺索引时自然路线保留关系计划，强制路线拒绝且下一语句恢复。默认成本、无校准profile、所有JOIN开启时，四层各32条平行边的终点集合自然选图，微型一跳自然选Hash Join；完整Normal计划均在专项目录。这证明选路机制工作，不证明成本预测已精准或所有图查询都胜出。

失败证据保留：首轮 `101035` 在十跳阶段重复引用的校验中崩溃，已改为解析完整引用链并加范围检查；自有失败实例fast停止超时后immediate停止，未删除现场。专项 `101348` 的16条平行边用例自然选关系：前一回归数据库有旧合成成本profile、全新数据库没有，暴露测试隔离不足；新回归使用独立角色隔离profile，扩大工作量以分别观察两种自然胜者，没有改低成本。原16条场景的保守估价漏选问题留给M6成本准确性评价，不能把它写成已优化解决。

M4验收点PG仍5d3d3c1，18项tracked修改＋10个新测试文件，tracked diff SHA256=`3a414c630a41e135eaa8478d126c6c5be605f886982df114ff60730595d4b0b2`；未提交，AGE clean，自有实例停止。复验入口 `pgq_btree_regions_v1.py`。自然模式仍支付关系DP/GEQO搜索成本，强制模式的规划收益不能套用给它。

## 13 M5 有界枚举实施设计

范围是已准入固定WALK的单路径区域，不扩为任意Cypher/PGQ。每区域最多8个起点/顺序候选：原顺序、反向顺序、至多3个估计起点行数较少的内部顶点各两种左右展开顺序；缺必要端点索引的候选不加入。额外阶段估价总量上限8192，且在候选间检查20ms软时间预算；长路径不做点边全排列。原顺序始终优先尝试，时间预算不能中断正在进行的语义检查。成本含真实起点、各阶段扇出/过滤、顶点检查和属性读取，不给予图强制折扣。

中间起点需要明确的绑定位置：结果数组仍按执行阶段存储，前端保存逻辑变量→位置映射；每跳读取已绑定的输入位置，而不是武断读取上一跳输出。父链保存完整路径，左右两支的匹配数量相乘，不能按端点集合合并路径历史。公共运行时新增运行期setter，不改变provider协议、磁盘格式或旧固定阶段struct。非线性候选关闭factor/endpoint-set/AGE快路径；原顺序可保留已经证明的优化。

闭合检查只提取安全的内置bigint顶点键等式：如果新顶点要求等于先前绑定的顶点，展开时仅保留符合该键的候选，仍交付每条真实平行边；原条件继续作为PG剩余条件复核。首版这是邻接读取内的早过滤，不宣称已对所有索引实现双键直探，也不把路径计数改成单个EXISTS。其他历史/属性条件仍按M2处理。

候选的图scope指纹包含执行顺序，不能复用另一顺序的阶段统计。Query逻辑IR不变，候选使用副本，缓存Plan只保存Node/OID/整数映射；DDL、角色、参数、错误和重扫沿用既有契约。EXPLAIN需显示选中起点、阶段输入/闭合位置及候选数，测量规划时间和执行时间分开。

验收须覆盖左右端/中间两种顺序、仅反向索引、各方向、属性/历史条件、平行边/自环/闭合、RR与重扫、预算和旧OUT快路径。不能为了测试枚举而修改数据语义或隐藏成本。

### 13.1 M5 实现与验收

正式 `graph_btree_max_candidates` 默认8、范围1～8；设1保留原书写顺序。比较两端及最多三个内部起点的左右两序，新增候选阶段工作量8192、候选间20ms软预算，不是整条SQL规划20ms的SLO。闭合检查保留完整物理边数量和原剩余条件；公共 `GgcFixedSetBindingPositions` 保存输入及匹配位置，非线性/闭合不进入factor/endpoint集合。仅强制B-tree通过公共策略接口保留图变量，避免PG self-join elimination合并闭合端点；普通关系与自然路线的SJE保持，受合并影响的自然图候选安全放弃，未声称该形态自然图也已覆盖。

| 检查 | 结果 | 证据 |
|---|---|---|
| O2配套构建 | PASS | [构建](../执行包/evidence/runs/20261021T103249.214032Z-build-pgq-integrated/summary.json) |
| 图模块，含新顺序/方向/闭合矩阵 | 19/19 PASS | [模块](../执行包/evidence/runs/20261021T103310.620269Z-G1-080-graphroute-recovery-modules/summary.json) |
| 独立起点/顺序/闭合/生命周期 | 12类 PASS | [专项](../执行包/evidence/runs/20261021T103530.735379Z-G1-080-btree-orders/summary.json) |
| PG / AGE原生 | 245 / 43 PASS | [PG](../执行包/evidence/runs/20261021T103501.066645Z-regress-pgq-integrated-pg/regression.log)、[AGE](../执行包/evidence/runs/20261021T103520.427897Z-regress-pgq-integrated-age/regression.log) |
| 属性 / 方向 / 多区域 / AGE开关 | 12 / 8 / 11 / 5 PASS | [属性](../执行包/evidence/runs/20261021T103527.043631Z-G1-080-btree-properties/summary.json)、[方向](../执行包/evidence/runs/20261021T103528.173847Z-G1-080-btree-directions/summary.json)、[区域](../执行包/evidence/runs/20261021T103529.372693Z-G1-080-btree-regions/summary.json)、[GUC](../执行包/evidence/runs/20261021T103532.094015Z-G1-080-pgq-integrated-guc/summary.json) |

新SQL矩阵含9种两跳方向×4种起点条件×4种历史/闭合条件，每种比较原顺序和预算8，另有参数缓存/REINDEX。独立专项分别观测中间先右后左/先左后右、左右端、只存在IN索引、同SQL预算1/8、RR并发写和真实三次重扫；src/dst、点名和数值边属性全部对照，不只比终点。256跳仍单个图节点，对照普通递归SQL的一条完整路径；完整Normal计划与SQL在专项目录，不拿该单次计时声明性能改善。

失败日志保留：GUC数据文件排序被构建器拒绝两次，已按实际字母序修正；专项 `102919` 的闭合路径被SJE提前折叠而拒绝，新增仅强制图的绑定保留策略后通过。没有关闭全局SJE让关系对照变差。未提交、AGE未改，所有自有实例停止；下一步M6。

## 14 M6 正式入口与收尾设计

先做机械迁移，再复验：将现有PGQ规划、GraphRoute、CustomScan及有界枚举从测试模块移入公共内核，后端启动注册，标准SQL不再依赖LOAD测试模块；默认自动选路off、AGE默认off保持。测试模块保留SQL诊断入口和故障provider，通过显式诊断接口接入，不能让生产内核链接测试模块的符号。迁移行数不记作新增算法。

公共层不能直接调用graph_adj的构建代数函数。增加独立、版本化的只读索引元数据回调注册，由实际AM/provider返回当前构建代数；不变更V2邻接读取、lease v4或V7页格式。C不要求B/A符号，B和A分别依赖C。测试模块原参数保留兼容，仅用于诊断调优；正式用户入口为图GUC和SQL/PGQ。

迁移后先验证不加载模块的标准语法、强制三路线、默认off/自然on、权限/MVCC/DDL/取消和所有既有回归，再按相同SQL测旧四场景与新增组合/选择率矩阵。规划与执行分列，DML只在相同索引和数据下比较开关/版本影响；不创建覆盖索引替用户承担写开销，不追逐倍率。四组合从隔离源码导出构建/回归，不改动当前分支或保护项目；最终核对设计摘要、实现边界、证据和交接文件，完成后停止。

### 14.1 入口迁移与性能反馈后的准入收紧

`graph_pgq.c`及两个include片段已迁入公共内核，`InitPostgres`注册PGQ规划/CustomScan；原测试模块只注册SQL诊断桥和故障回调，没有内核到测试模块的链接。`GgcIndexMetadataOps`为单独版本的只读构建代数回调，实际adj AM注册；原邻接V2、lease v4、固定执行V2、V7页格式不改。迁移行数不是新增执行算法。

首轮性能矩阵发现AUTO在外围强过滤JOIN中保留图边界，令原生条件下推失效：0.024ms的原生关系执行变成约21.864ms。已据此收紧AUTO：存在外围JOIN区域时完全保留原生规划，不安装图边界策略；显式B-tree仍可组合外围JOIN，由用户承担选择的性能。独立图查询、无跨区域JOIN的集合操作仍可自然比较。这里没有实现全查询两族规划比较，不再把“受图边界限制的关系候选”等同于原生完整候选。该限制是M6对M4范围的最终修正。

兼容回归还发现方向扩展意外放宽 AGE 混合方向准入，已同时在规划和缓存计划执行期恢复其全 OUT 限制。AGE 源码及低层邻接接口未修改；旧13组语义测试和新增缓存计划 GUC 切换测试通过。不能用 B-tree 的方向支持替 AGE 未验收语义背书。

### 14.2 执行包设计基线审阅

原manifest登记摘要 `b32dae139741d69121a532c841712a5a776e00556d71e2acb19cf19f53367607`，本轮开始前主设计已变为 `7cd3cd2f8adeaf800c4760108c7eb6e120a8605a6780e051febd77c2a14ec50f`。未找到原摘要对应的完整设计快照，故不声称逐字证明了全部历史差异。本轮按当前文本与任务断言重新审阅，更新摘要只绑定审阅对象，不宣称功能门通过。

| 主设计契约 | 本轮处理与保留边界 |
|---|---|
| §3、§9的binding多重性、MVCC、权限和预算 | M2～M5保留完整bag；仅既有已证明的端点集合使用去重。无spill的硬预算触限明确ERROR，不截断或暗中加预算。 |
| §4.5、§10的单向依赖及版本化provider | C不链接B/A或AGE私有符号；索引元数据单独版本化；四组合实编译/回归。历史参考头不是已安装API或最终磁盘字节。 |
| §4.6、§6的图/关系组合、有界候选 | 正式入口与强制组合、混合方向、中点有界枚举已实现；外围JOIN的AUTO保留原生，未实现跨边界全局竞争/任意参数化图区域。 |
| §7～8与§15.9的统计/代价 | 不改PG/AGE原统计语义，不在DML内收图统计；缺失估计仍有明显漏选，报告Q-error/regret，不据强制成绩签收自然CBO。 |
| §10的稳定引用和紧凑存储目标 | 本轮不变更存储协议、格式或WAL；架构目标仍保留，不因B-tree演进删除adj后续门。 |
| §14的2～4/2～8跳阶段 | 执行包batch契约已明确它们是验收覆盖，不是运行期上限；本轮256固定跳功能通过，不声称可变长/shortest已实现。 |
| §15.7的DML零退化、生产SLO和长期稳定门 | 不放宽margin=0，不填写未经用户确定的业务SLO；本轮写入开关筛查不等于提交吞吐/P99非劣化。 |

`tasks.json`、`acceptance.json`的任务依赖和强制断言保持，`task_state.json`不修改。历史V1参考摘要不改。主设计仅补当前状态导航；基线manifest新增本节审阅说明，离线校验结果在收尾记录。

### 14.3 M6 收尾结论

[完整阶段报告](../报告/图Btree组合执行M0-M6验收报告.md)汇总最终构建、未提交源文件摘要、图19/AGE43、GUC6、AGE语义13、adj安全9、无模块7组、批访问673、覆盖224＋65和四组合各PG245证据。可使用正式入口，不再依赖测试模块；默认自动图路线/AGE仍关闭。

同SQL选择率矩阵七场景中五个达到三干净轮。枚举避免了书写起点扫描过多甚至预算超限，但不能保证比关系执行快；外围强选择性JOIN的AUTO已恢复完整原生路线。自然CBO仍会漏选高收益集合路径，属性读取也可能更慢。DML四项开关筛查执行变化−2.74%～+5.34%、WAL中位相同，未证明严格零退化。原四类十跳语义均正确，计时因宿主机换页/major fault均未达三干净轮，不宣布历史倍率重新验收。

本轮有限范围已执行完，停止追加实验；不关闭正式G1-080或生产门，不提交/改写commit。失败、污染轮与原始数据保留，全部自有实例停止。后续若继续，应由用户另行选择CBO、跨边界全局比较或正式性能门，而非把旧“持续10×”当成自动续做授权。

## 15 原建议范围内的续做清单

用户在上述收尾后明确要求继续剩余开发和测试。本节覆盖已发现的估算问题和原验收缺口；不追加任意图模式、graph_adj存储优化或AGE缓存扩展。默认开关、结果语义、内存上限及正式生产门不变，不为接近早期代码量估计增加实现。

- [x] **R1 估算修正。** 严格消除重复输入等值条件，已知键利用PG统计，B-tree缺失物理统计兜底复用PG索引计价；确定性200行估算回归通过。右端/跨跳相关性仍不准，16平行边AUTO仍漏选，不承诺任意数据最优。
- [x] **R2 功能与独立性回归。** 同SQL完整bag/方向/属性/生命周期、无模块7组、图19、PG245、AGE43、批访问/覆盖/安全及四组合各PG245通过。普通DML不进入自动图程序搜索。
- [x] **R3 性能复测执行与复算。** 七场景各3轮，4/7达门；原四类各12轮，高汇聚5轮干净通过，单链/连续/稀疏为2/2/1，仍未通过。全部污染轮保留，没有补跑或放宽门。
- [x] **R4 写入影响测试与复算。** 旧提交独立O2构建对照当前off/on，4,608个真实提交事务，行数/全表汇总/WAL核对；另有48条回滚筛查。区间均跨1，且保留DELETE短样本+27.8%异常，不签收严格零退化。
- [x] **R5 报告归档。** [报告第8节](../报告/图Btree组合执行M0-M6验收报告.md#8-原建议范围内的续做结果2026-10-21)、性能§2.22及交接入口记录当前构建、通过/未通过项。正式门不改为通过，不commit/amend。

以上R3/R4勾选仅表示固定测试和分析工作已经执行，不表示性能门通过。尚未签收的正式门：自然CBO预测精度、三个无重复场景的干净复测、DML严格零退化与生产并发/尾延迟。当前默认AUTO与AGE仍off，G1-080仍in_progress。

前置离线校验已通过；续做前安装和两个入口文件备份位于 `.build/pg19-graph-core/m6-followup-baseline-KEIaYN/`，对应首轮110527构建。`pgq_btree_costing_probe_v1.py`记录修改前后真实标准PGQ计划和完整结果，不把诊断计时当性能结论。估算修正后固定12轮原四场景复测，至少3个完整干净轮才发布稳定倍率；若仍不足，报告环境缺口，不修改门槛。

续做测试前固定DML方案：`pgq_btree_dml_committed_v1.py`比较已提交的`5d3d3c1`（独立O2构建）、当前版本自动图开关off/on三个独立实例；每段恢复相同10万行和普通主键/src/dst窄索引，执行16个各改100行的真实提交事务，四类操作各12个旋转镜像轮，保留所有样本。计时包含客户端BEGIN、DML和COMMIT，不混称纯执行器时间；另存TEXT EXPLAIN诊断。至少3个整轮无宿主机换页/major fault才报告探索性配对区间，仍不等于生产并发吞吐/P99或严格零退化门。没有新增adj、覆盖索引或AGE写维护。

R1实现使用只读PG统计：每一阶段只移除已由输入键与拓扑严格蕴含的重复等值条件；已知输入常量用该列选择率，而非全表平均度。缺少图物理统计和标定profile时，调用PG原生`cost_index`计实际索引访问，缓存重复次数参考普通参数化索引路径的`get_loop_count`口径；再加图框架、绑定、过滤和属性读取费用。带图统计/标定profile、adj和AGE保留原计价分支。不会写入PG/AGE统计，也没有给图路径附加经验倍率折扣。

## 16 三项未签收问题的定向续做

用户继续要求处理AUTO漏选、三个无重复场景计时和DML零退化。本轮实现与测试见[报告第9节](../报告/图Btree组合执行M0-M6验收报告.md#9-三项未签收问题的后续验证)，当前构建为142315；第15节保留前轮结果。

- [x] **T1 定位AUTO淘汰。** 冻结数据/PG统计，GDB确认JOIN层普通支配规则在DISTINCT之前丢弃全局唯一图候选。
- [x] **T2 保留安全候选。** 每区域至多保留一个独立CustomPath，仍须原单起点及终点集合唯一性证明；到UPPERREL_DISTINCT再与完整关系路径正常比较，不改通用add_path、不打折代价、不放宽bag语义。
- [x] **T3 回归。** 单/多/空起点、ORDER/LIMIT、COUNT bag、generic/custom参数；图19、PG245、AGE43、无模块7组、GUC6及四组合各PG245通过。初版NULL解引用已由fresh initdb发现并修正，失败日志保留。
- [x] **T4 无重复复测。** 预定各12轮，单链/连续/稀疏分别4/8/5轮有效，达到至少3轮门；图B-tree执行提升7.76/5.14/2.21×，同SQL、完整结果、全部240489条计时已复算。
- [x] **T5 DML定向验证。** 1536条DELETE回滚真实/假对照；73728个真实提交事务、每段256事务。固定轮数、相同索引/持久性，逐条行数、全字段指纹、WAL和配对区间复核完成，不改零容忍门槛。
- [x] **T6 报告与交接。** 当前源文件/二进制身份、最新性能和未签收项更新到原文档，旧记录与失败证据保留。
- [ ] **验收项：AUTO全面准确。** 本轮修复确定的候选生命周期缺陷；其他统计快照仍可能估价不准，不能把自然CBO全部签收。
- [ ] **验收项：DML严格零退化。** 长窗口DELETE未复现+27.8%，但部分区间仍跨1、回滚假对照仍有未归因差额；没有生产并发/尾延迟/长期门。

T1～T6勾选表示限定开发、验证和归档已执行，不替代后两项验收。默认AUTO/AGE仍off，正式G1-080不关闭，不commit/amend。后续优先研究跨跳相关性与集合基数，不以强制图成本折扣修饰AUTO；写入若继续，先定位差额、改善可重复测量条件，再决定是否需要内核改动。

### 16.1 主设计状态导航校核

主设计仅第3行状态导航更新到本轮实际结果，架构正文、统计/存储契约和验收门未改。将此行还原后，全文SHA256精确恢复为修改前`e77ff224dd3fb067f6425689cd43c9cb14592bcbb3e39a2134b1126f95e0c3e2`；新全文为`6c23b72814b23adc277e831c2f9276b6c4b0565a4af2ca138995477e400cd75c`。因此仅同步manifest绑定摘要和本节审阅入口，原架构实质审阅仍见§14.2；任务依赖、强制断言和task_state不变，不以摘要刷新代替任何功能或性能验收。

## 17 DELETE诊断与无hint规划优化

- [x] D1 固定CPU/调度诊断和OFF/ON火焰图，确认普通DELETE没有图执行分支，不能把全部差额归为等待。
- [x] D2 定位所有普通SQL的冗余hint复制/词法解析；不存在`/*+`时提前返回AUTO，出现标记仍完整校验。只改解析判断，不修改heap/MVCC/WAL或强制hint语义。
- [x] D3 增加AUTO off/on的DML、引号/注释假hint和强制DML拒绝回归；配套O2构建、图19、PG245、AGE43、无模块7组、GUC6通过。
- [x] D4 前后固定12288条测量及73728个真实提交事务，逐轮复算、全部污染和不利样本保留。无hint规划约减少2%～3.5%，不称纯执行收益。
- [x] D5 验证prepared对照有效性：逐条VACUUM时8/8重新规划，停止后0/8；该组不能排除规划影响，明确撤销该用途，不改旧样本。
- [x] D6 四组合独立迁移复验：图2/15/4/19、各PG245通过；当前身份、所有原始证据与续做边界归档到原报告和交接入口。
- [ ] 原DELETE纯执行差额的具体根因；目前没有证据支持修改普通删除执行器。
- [ ] 整体DML严格零退化：INSERT新信号和DELETE区间仍不满足，不改门槛、不用不同时间指标替代。

后续若继续，先在同backend、真实缓存复用的条件下拆分CPU与COMMIT刷盘等待，再决定产品改动；不无限重跑同一窗口。源码、数据、当前边界和失败证据见[报告第10节](../报告/图Btree组合执行M0-M6验收报告.md#10-delete差额诊断与无hint解析优化)。本轮不修改主架构正文及其绑定摘要，不变更任务依赖或正式G1-080状态。

## 18 PGQ 接入补齐 Top 3（2026-10-22，限定范围已完成）

用户要求从尚未接入的 PG19 图能力中选择三项补齐；本轮以图 B-tree 为主，不扩张 AGE/adj 的准入范围，不改变默认开关。

| 项目 | 实施范围 | 关键安全约束 |
|---|---|---|
| T1 异构表路径 | 每跳分别绑定输入点、边、输出点的原表和索引，支持一条路径经过不同点表、边表 | 同值 ID 不等于同一种顶点；可见性、属性读取按对应原表和同一快照检查 |
| T2 键与物理列布局 | bigint 单列键可不在第一列；支持合格的即时、全表唯一 B-tree；边表可只有 id/src/dst 三列，不再要求第四列 | 仍要求非空 bigint；文本/复合键不在本轮；部分、表达式和延迟唯一索引不能充当身份证明 |
| T3 复用顶点变量 | 固定路径中的 `(a)->(b)->(a)` 等闭合绑定 | 输出点必须等于已绑定的同一变量；保留平行边及路径的 bag 数量，不启用端点集合去重 |

实现沿用 `GgcFixedProgram` 的实际属性编号及每跳 `GgcFixedStagePlan`，计划中序列化完整映射；保留旧测试入口格式。重新选择执行顺序后，按该候选的每跳映射选索引，不能复用第一张边表的索引。属性读取索引及每跳索引纳入计划失效依赖。新映射的统计指纹区分列和拓扑，不把同构路径的统计套给异构路径；缺失时只读 PG 列统计回退。

- [x] T0 范围及代码复用点检查：底层已有每跳映射、属性可见性读取和 binding-position 校验接口。
- [x] T1 每跳表映射、索引选择及代价估计接通。
- [x] T2 非固定列位置、三列边表及单列唯一键接通。
- [x] T3 复用顶点的闭合约束接通，包括两变量自环；校验全部图变量确实由阶段绑定。
- [x] T4 相同图 SQL 的 relational/B-tree 完整 bag 对比，另用普通 SQL 独立校验；覆盖过滤、属性输出、方向、预备计划及失败回退。新增238个正向矩阵调用、6个不准入检查；两会话专项13组通过。
- [x] T5 配套 O2 构建、图20/20、PG245/245、AGE43/43及GUC6/6通过。验收绑定101239构建，不沿用历史二进制。

不宣称所有 PGQ 能力已接入，也不承诺未测性能倍率。RLS、子查询/相关参数和其他已有安全边界保持不变。账户剩余额度≤10%时按用户要求立即停止。

实现位置：`graph_route_hint.inc`逐跳挑选访问索引；`graph_pgq.c`准入、统计回退、序列化实际表/列及输出属性映射；`rewriteGraphTable.c`隔离新映射/闭环统计指纹，并按原生PGQ的图元素归属确定异构无向模式的合法访问方向。复用`GgcFixedSetBindingPositions`执行闭环约束，未新增存储格式或更改B-tree provider算法。原生普通PG算子仍可包住图区域；不能证明安全的表达式和表类型仍拒绝强制图路线。

## 19 第二轮 Top 3 接入（2026-10-23，限定范围已完成）

用户要求继续选择三项。本轮核对底层契约后选择如下可独立验收的范围，不把通用键或RLS的准入检查简单删除：

| 项目 | 本轮目标 | 不扩大的边界 |
|---|---|---|
| U1 常用整数键 | `smallint/integer/bigint` 单列非空键，含跨整数类型连接；内部保留精确int64绑定，B-tree探测显式使用原索引类型与int8查询值的交叉类型操作符，输出仍保持原SQL类型 | 不支持文本、UUID、复合键或可空端点；adj/AGE不随之放宽 |
| U2 列级SELECT授权 | 不必授予整张表SELECT；图运行所需键列、实际引用的属性及过滤列分别校验，启动/重扫描保留权限检查 | 不允许通过内部回调或延迟属性读取访问未授权列；RLS仍拒绝图路线 |
| U3 边表多图元素 | 保存PGQ重写选中的精确图元素OID，同一物理边表可通过不同图别名、端点映射参与固定路径 | 不把物理表OID当作图元素身份，不改变bag重复语义；`f\|r`多标签产生的UNION分支仍走relational，保留负向测试；adj/AGE仍保留原映射限制 |

优先复用现有公共执行层，不修改页格式/WAL/普通DML。C层与B层保持单向依赖。整数键必须覆盖负数和边界值，避免通过窄化转换制造错误匹配；批探测、覆盖读取、顶点可见性和属性索引必须使用一致类型规则。列权限由原生Executor权限检查兜底，额外图访问还要检查自己的读取列，不将“同一表出现在查询中”当作任意列读取授权。

- [x] U0 当前源码/dirty/服务/数据保护核对，明确上述范围与回退。
- [x] U1 整数键实现及类型/越界/方向/批处理验证；实测确认边/点数组批查和范围扫描、覆盖/回表、DESC回退分支被执行。
- [x] U2 列权限实现及真实非superuser、跨会话撤销/恢复权限、缓存计划验证；原始provider调用未声明有限读取契约时仍要求整表SELECT。
- [x] U3 精确图元素映射及双别名/不同映射/属性DDL后的缓存失效验证；多标签UNION仍拒绝强制图路线。
- [x] U4 相同图SQL仅换hint，完整bag和原始SQL类型对照，普通JOIN SQL独立校验；新两会话专项30组通过。
- [x] U5 配套Release构建、图21/PG245/AGE43、GUC6、前轮映射13、AGE语义13、adj安全9通过，报告和交接已更新。

完成勾选仅表示本轮限定范围，不签收AUTO全面准确、DML零退化或未测性能；用户额度≤10%立即停止。

实现补充：`graph_access.h`统一整数精确扩宽、原类型输出、交叉类型ScanKey和有限列权限检查；`graph_fixed.c`覆盖普通/因式化/端点集合三种交付，不把窄键强转后绕回。`GgcVariable.element_oid`保留准确图元素身份，统计指纹区分新别名/类型；不修改同一SQL的bag数量或路径方向。有限列权限通过显式`GGC_CAP_COLUMN_ACL`契约选择，RLS仍独立拒绝。公共语义版本和固定执行版本升至3；邻接provider V2、lease V4、adj磁盘V7不变，PG/测试模块/AGE须配套重编。四组合独立迁移本轮未复验。

## 20 九类原生能力接入（2026-10-23，限定功能已完成）

本节是用户在第二轮 Top 3 之后新增的范围，覆盖前述尚未接入的九类能力，不沿用旧“限定范围完成”结论。目标是同一图 SQL 仅切换 GraphRoute，结果类型、NULL、完整 bag、权限与快照一致；不能把普通关系回退记作图执行器接入。原 PG19＋ORCA＋Daft 安装和实例不动，现有未提交改动全部保留。

### 20.1 实现选择与安全边界

保留 `GraphExpandFixed` 的整数键、批量访问和安全终点集合快速路径。增加通用类型的逐阶段图执行路径：图程序约束变量绑定顺序；每个变量的实际读取复用 PG 生成的参数化扫描子计划，最终由一个图执行状态机驱动，不先生成整图关系 JOIN 计划再换名称。

1. 原生索引路径决定键比较、collation、部分索引谓词证明、表达式匹配及成本。键按原 Datum/类型传递，复合键逐分量等值，不压成 int64 或无碰撞保证的哈希。
2. RLS/security-barrier、分区/继承、视图及 FDW 保留原生扫描子计划和权限元数据。图执行器不越过安全过滤读取“原始邻居”。没有可用 B-tree 时，计划必须明确显示实际扫描，不能冒称免回表或索引直达。
3. 逐阶段参数由图节点管理，外层 LATERAL 参数仍由 PG 管理；计划复制/序列化、参数依赖、rescan、取消和结束清理均纳入测试。每阶段只保留当前 binding 元组，不缓存事务外邻接。
4. 通用路径保持完整 bag；不自动借用整数快速路径的端点去重证明。复杂属性、跨历史条件由原生表达式求值，聚合、窗口、SRF 留在 PG 规定的层次，不越过安全边界或丢失路径历史。易变函数遵循 PG 求值规则，不承诺不同物理计划的调用顺序相同。
5. 多标签保持原生 UNION ALL 分支及其重复语义，各分支独立接入；不把多分支压成一个不准确的图程序。
6. 不扩张 SQL/PGQ 本身的语法边界：原生拒绝的 GRAPH_TABLE 内子查询仍拒绝。外表只能承诺通过验收的 FDW 组合；远端事务/快照不可能被本地执行器自动升级为更强保证。

通用路径首先以强制 B-tree 路线验收。它改善能力覆盖，不承诺达到原整数批量路径的性能；AUTO 必须有真实成本候选才能接入，不作强制折扣。通用路径使用原生逐阶段访问的事实须在 EXPLAIN 和验收报告中写明。

### 20.2 执行清单

- [x] N0 核对源码、dirty 修改、安装身份、服务状态及九类原生能力入口，记录设计与安全边界。
- [x] N1 通用键/逐阶段图执行：保留复合映射，原类型参数、属性输出、完整 bag、计划生命周期；非确定性 ICU 排序规则相等性通过。
- [x] N2 NULL 端点：OUT/IN/无向、孤儿边和复合键任一分量 NULL 与 relational 一致。
- [x] N3 RLS：点/边策略、角色切换、FORCE RLS、策略变更、拒绝权限及 security barrier。
- [x] N4 分区/继承：父子表、运行期分区裁剪和跨分区更新后的读取；子表访问由原生规划决定，不要求统一索引。
- [x] N5 视图/外表：普通与安全视图、视图属主授权；实测 file_fdw 读取/rescan，不承诺任意 FDW/存储。
- [x] N6 LATERAL：多/空/NULL 外层输入、外连接、prepared generic/custom 计划及 rescan。
- [x] N7 多标签：点/边标签备选、多个 UNION 分支、重复/属性类型与同 SQL 对照。
- [x] N8 部分/表达式索引：合格与不合格谓词、原生表达式/类型/排序规则、计划失效及真实选用证据。
- [x] N9 复杂表达式：合法标量/域/属性表达式、外围聚合窗口、COLUMNS 内/外围及嵌套 SRF；原生不支持的内部子查询双方拒绝。
- [x] N10 配套 O2 构建、新专项61、图22/PG245/AGE43及旧专项/AGE/adj安全回归通过；报告与交接已更新，不把未测项签收。

### 20.3 实现落点与区别

`GraphExpandNative` 是新通用路径的 EXPLAIN 名称。`rewriteGraphTable.c` 保留完整键列列表及 `native_access`；`graph_pgq_native.inc` 按图变量顺序选择原生扫描 Path，使用临时 NestPath 做访问成本及条件归属计算，但不生成图区域的 Nested Loop/Hash/Merge Join Plan、不做 DP/GEQO 子集枚举。它不是把现成关系 JOIN 改名为图节点。

`create_graph_stage_plans()` 在创建计划时分配阶段参数；`CustomScan.custom_params` 声明由图节点提供的 PARAM_EXEC，`subselect.c` 只从图节点的外部依赖中扣除这些内部参数，仍保留真正的外层依赖。执行阶段按深度推进、退回、重扫子计划，每层保留一份当前元组，使 text/jsonb/TOAST 等引用值在游标暂停及后续绑定期间有效。没有执行期重新规划 SQL，也不缓存事务外图数据。

SRF 修复位于 `generate_query_for_graph_path()`：生成图子查询时按实际输出设置 `hasTargetSRFs`，由原生 `ProjectSet` 负责一条路径产生多条输出。不能让 `ExecProject` 把多行函数当成单值。完整案例及最终测试见[报告第16节](../报告/图Btree组合执行M0-M6验收报告.md#16-九类原生能力接入2026-10-23)。

图语义版本升至4，固定整数执行版本仍3，provider V2/lease V4/adj磁盘V7不变；修改了公共 Query/Plan 结构，PG及扩展必须配套重编。默认AUTO/AGE仍off；新增通用路径只由强制B-tree接入，AUTO未扩大。没有本轮新性能倍率、DML零退化或四组合迁移结论。

账户余量 2026-10-23 01:43 UTC 实读约86%；后续继续检查，≤10%立即停止。完成范围以本节和报告的实际验收边界为准，不代表PGQ任意语法或生产门全部完成。

## 21 当前源码的四组合独立迁移复验（2026-10-23）

用户在第20节收尾后要求完成四组合后续验收。本轮仅验证公共C、C+B、C+A、C+B+A的可独立移植性，不扩展到AUTO、性能或生产门。原开发树、Git历史、集成安装及PG19＋ORCA＋Daft保持不动；脚本`pgq_btree_portability_v3.py`导出当前完整dirty快照，并纳入未跟踪的新内核和测试文件。

不只在副本中排除provider：从当前分层源码生成C/B/A三个补丁，然后从干净PG19基线`7873db5`重放四种组合并逐文件核对。额外验证C+A+B与C+B+A完全相同。实际编译的必须是补丁重放所得源码，不能只验一个完整二进制后凭开关推断独立性。该验收是独立补丁移植，不改写现有六个commit，也不称为新增commit的cherry-pick验收。

- [x] P0 核对源码/dirty、服务、保护安装、实际余量，冻结验收来源。
- [x] P1 分层补丁生成、四组合重放及A/B顺序交换的一致性核对。
- [x] P2 各组合独立Release/O2编译、按实际fragment核对完整图回归集合、各自PG245回归。
- [x] P3 二进制符号和catalog检查；无测试模块LOAD的标准PGQ正/负向验证，缺provider明确拒绝；通用类型/SRF只在含B时进入GraphExpandNative。
- [x] P4 核对原源码/集成与保护安装未变、停止所有自有实例、保存补丁/日志并更新报告和交接；执行包校验及新增段落22条本地链接检查通过。

不签收大数据性能、DML零退化或任意FDW；每个组合必须有自己的新日志、二进制摘要和完整测试列表。回归预期为C=2、C+B=18、C+A=4、C+B+A=22，另各245项PG原生；数量还须与实际测试名字逐项核对，不能只改计数让测试通过。账户2026-10-23 02:03 UTC实读84%，≤10%立即停止。

最终[021421矩阵](../执行包/evidence/runs/20261023T021421.092806Z-G1-080-provider-portability-v3/summary.json)四组合全部PASS，图回归实际2/18/4/22，各PG245/245；额外无模块入口及符号检查均通过。补丁、测试边界、首次夹具失败和来源保护见[报告§17](../报告/图Btree组合执行M0-M6验收报告.md#17-四组合独立迁移复验2026-10-23)。本轮未修改内核或现用014550安装，未commit/amend/push。

## 22 AUTO、DML与生产门续做（2026-10-23）

用户要求继续处理三项未签收门。本节为本轮工作清单，不预先承诺通过，也不将工程筛查改名为生产准入。默认AUTO/AGE开关不变，不改保护项目、既有数据、提交历史或旧失败证据。

- [x] A0 核对当前源码/014550安装、既有AUTO及写入负面证据、正式验收契约；账户余量02:47 UTC实读81%，≤10%立即停止。
- [x] A1 冻结数据和PG统计复现AUTO选路与基数误差，区分候选遗漏、估价错误、超出准入范围；不按测试答案硬编码选择。025257复现32/48平行边漏选。
- [x] A2 修复条件集合统计采集与消费，保留关系候选及安全回退；新通用路径AUTO仍不开放，剩余选优反例见A4。
- [x] A3 041549完成10场景/7边界，记录真实q-error、零结果误差、选路、规划/执行及regret，失败/污染轮全部保留。042548逐原始记录复算通过；仅验证执行完，不等于选优门PASS。
- [ ] A4 统一两路线条件基数和批访问成本，解决三叉树仍选错的反例，为GraphExpandNative建立真正的AUTO成本竞争；独立校准/留出先预注册，不能硬编码折扣。**其中三叉树反例及有界批计价已在§23完成；通用GraphExpandNative AUTO仍未开放，因此不将本项整体打钩。**
- [x] D1 核对DML基线与控制变量，冻结轮次/指标/统计方法；分离纯执行、规划和持久提交等待，包含OFF/OFF假对照，不将无显著差异当成零退化。`pgq_btree_dml_phases_v3.py`固定12镜像轮×256事务/段、4操作及同旧backend假对照；只作工程筛查，不满足生产样本量。
- [ ] D2 按冻结协议完成读写配对、吞吐/P95/P99及必要维护观测；只有实际有代码根因时才优化，不为消除宿主IO波动修改图存储或关闭持久性。
- [ ] P1 确认业务范围、负载/SLO和资源预算，补齐正式预注册。当前`gate.py check business-input`明确缺12项业务字段；已向用户询问仅B-tree或含adj/AGE及业务参数，未自行填值。
- [ ] P2 依赖P1执行生产门：正式契约至少20独立配对轮、每轮P99至少10000成功事务、5 checkpoint及3完整维护周期，比较族校正、维护债务、安全/生命周期全部通过；零退化容忍度仍0。
- [x] R1 图23/PG245/AGE43、GUC6/AGE语义13/adj安全9及040829四组合各PG245通过；全部自有实例停止，042548复算保留NOT_SIGNED，报告/交接和最终核验见报告§18。

正式业务输入缺失不阻塞A1等独立工程工作，但阻塞P1/P2及正式生产签收。旧G1-080验收脚本仍明确拒绝用合成成本或写死q-error验收；本轮也不能仅将状态字段改成PASS。任何新验收脚本都须校验真实原始记录、构建身份和独立留出来源。

### 22.1 本轮AUTO修复边界

冻结100000边/表、完整PG统计的四跳诊断中，32/48平行边场景AUTO选择关系路线；已有的单起点统计接口未被正式PGQ Path使用，采集的下一层不同端点数也未进入成本消费。不能用降低图成本常数来掩盖这两个缺口。

新增独立`result_mode=2`端点集合统计，仅用于WALK；原`result_mode=1`完整binding统计不变。采集按每个起点独立去重后再展开下一层，真实记录候选、回表、页组及下一层端点数；新增metric 210，不把“平均去重比例×平均路径数”冒充端点期望。预算/取消/MVCC/权限和PARTIAL机制保留，不扩大原采集器2～4跳、同构bigint表的支持边界。无属性过滤用attnum/clause_id/min/max全0的唯一编码，半指定的过滤参数拒绝。

PGQ只在已有端点集合语义证明成立、规范顺序、同元素同方向、唯一整数锚点被常量等式约束时调用单起点统计接口；键、角色、图、方向、结果模式、指纹/代际须完全匹配。多起点/generic参数不借用单起点样本；缺失、截断、失效继续回退。新通用`GraphExpandNative`的AUTO准入本轮不通过删除限制强行扩张。测试专用scope检查函数只用于注册夹具，真实计划/性能在不LOAD测试模块的新后端验证。

源安装014550已冻结到`.build/pg19-graph-core/auto-stats-before-odcYiv/install`，PG SHA256仍`ff0d8af078535aada30001be58440052519e47ff73a6277d47cb439a66648096`。本轮不修改DML执行、heap、索引维护、WAL或提交逻辑。

### 22.2 已发现且不能隐藏的反例

032107留出实验中，单链/重复路径的估算改进，但三叉树端点数从错误的3修正为81后，图路线成本77.75大于关系路线31.78，AUTO选择后者；关系路线自身仍估3行。实际计划显示图路线33次buffer hit、关系路线331次，执行器确实批量访问。图成本当前按逐键probe计价，原生`selfuncs.c:btcostestimate`对SAOP的根下降次数也明确采用保守估计。不能靠只改图成本常数、删掉关系候选或把关系行数直接乘上经验倍数来签收AUTO。统一条件基数消费与批访问成本校准仍待完成；通用类型AUTO也未因此接通。

该轮结束时PG统计保护检查失败。033746只读克隆审计确认旧种子顶点表`v`被auto-ANALYZE刷新了直方图（11:21:38北京时间），不是图采集改写PG统计。修复测试夹具：只在克隆上关闭该表自动维护，冻结原统计，不修改用户种子；032107保留为失败/诊断，不能当正式冻结性能证据。48平行边关系计划另有外部排序落盘，即使重复运行也不能当全内存倍率。

034327配套O2已通过图23（新增`graph_auto_stats`）、PG245、AGE43、GUC6、AGE语义13及adj安全9；包括单/多起点、错误常量、bag、generic、部分统计、缓存代际失效和REINDEX回退。语义通过不等于AUTO全面准确或生产签收。

### 22.3 DML工程筛查与正式门的区别

034754固定协议已跑完98304个测量提交事务，所有数据指纹相同。INSERT/属性UPDATE/端点UPDATE/DELETE的有效轮数为7/9/9/11；新版ON对旧版OFF端到端比为0.9550/1.0168/1.0196/1.0505。DELETE探索区间[1.0163,1.0878]是不利信号，不能以“其余区间跨1”掩盖。

原摘要中的“maintenance disabled”不准确：只关了autovacuum，WAL仍触发checkpoint，旧实例3次、新实例各1次，且旧backend还承担假对照的双倍重置/WAL。这个因果实验存在维护状态不平衡，`clean`只表示主机无swap/major-fault。保留所有慢轮，不把这次结果签成零退化，也不凭代码未改COMMIT就说写入没有劣化。下一次先冻结同维护状态、每段checkpoint/WAL时点观测及完整基线定义，再做预注册因果对照。详细区间见报告§18.2。

### 22.4 验收脚本错误留痕

035710已修复PG统计漂移，但最终“无测试模块”检查失败。源码核实：`fmgr_c_validator()`在`CREATE FUNCTION ... LANGUAGE C`时调用`load_external_function()`；不是只要不写LOAD就不会加载。此前把辅助函数定义放在查询会话是错误的，032107/035710均不得作为无模块性能验收依据。修复后定义和调用都放在独立管理会话，主查询会话在创建后、每个场景计时前后及最终检查`/proc/<pid>/maps`。既有失败记录不覆盖，也不修改预定10场景、6轮、7次/段的协议来缩短或挑选有利数字。

最终041549工程验证完成：32/48诊断和36留出改为选图；三叉树仍选关系，AUTO执行0.12425ms、强制B-tree0.06336ms，未签收。48关系排序落盘，不列全内存倍率。042548从原始时间/计划复算，DML负面信号及生产12＋5缺项均保留；状态为`AUDITED_NOT_SIGNED`。测量时DML脚本已归档在审计目录；之后只修正活动脚本“maintenance disabled”的措辞，不改变旧测试、数值或门槛。

## 23 三叉树AUTO与DELETE因果优化（2026-10-23，限定工程工作完成）

本轮范围为用户明确指定的两项，不扩展通用类型AUTO或生产准入，不修改默认开关、DML持久性、PG/AGE统计语义或旧证据。源码基于034327版本；原安装冻结到`.build/pg19-graph-core/auto-batch-before-YtJ8KO/install`。首次额度查询网络失败，06:51 UTC恢复读到69%，≤10%立即停止。

- [x] O1 冻结原安装、查明逐键计价及不对称条件基数；保留064225/065123诊断。
- [x] O2 原生JOIN在路径生成前消费匹配的有界条件统计；所有路线使用相同语义范围，不以成本折扣强制选图。原生DP可延迟无过滤顶点检查，因此覆盖相同边前缀而不局限于完整点边交错前缀。
- [x] O3 有界批量索引成本使用实际数组请求，尾批不能按满批多收；普通PG成本仍保守，不把“改为数组估算”本身当性能优化成功。
- [x] O4 071004同SQL、标准PGQ、冻结PG统计复测原10场景；增加二叉、四叉、稀疏三叉、汇聚和错误锚点/过滤边界，generic由模块回归覆盖。14场景均选B，其中48仅核对计划/结果，另13例有有效热态计时。不改用户种子，不挑慢轮重跑到PASS。
- [x] D3 071455单一物理数据目录交替旧OFF/新OFF/旧假对照/新ON，每段相同10万行、256个100行DELETE提交，先完成CHECKPOINT；直接SQL和真正generic预备语句两组，各12旋转镜像轮。完成49152个测量事务，另列4096事务smoke；192计时段均无checkpoint，direct12/12、prepared11/12有效，未关闭持久性。
- [x] D4 072116从原始记录独立复算，篡改耗时/checkpoint摘要的负向检查被拒绝。普通SQL新ON/旧OFF比1.0223 [0.9784,1.0915]，预备语句0.9960 [0.9587,1.0417]。剩余正差额主要在WAL同步等待，不冒称DML内核优化或零退化签收。
- [x] O5 070256配套O2、图23/PG245/AGE43、GUC6/AGE语义13/adj安全9及072836四组合各PG245通过；停止自有实例，更新报告、性能汇总及交接。

条件统计首先尝试完整binding模式；只有没有完整binding统计、集合统计READY且此前每层`accepted_edges == next_endpoints`时，才把该样本的集合数用于无汇聚前缀的路径基数估计。一旦观察到汇聚即停止借用；不会据统计消除JOIN/DISTINCT或改变bag执行。缺失、部分、失效、多起点、generic、RLS、额外属性过滤仍保持原估算。逻辑统计读取不依赖B-tree/adj provider，C层独立性已由072836四组合验证。

最终[报告§19](../报告/图Btree组合执行M0-M6验收报告.md#19-三叉树auto修复与delete受控对照2026-10-23)及性能§2.28为本轮结果。三叉树AUTO已选图，纯执行0.074ms、关系0.14236ms、强制B-tree0.05889ms；AUTO/最快路线比1.26，规划2.709ms仍慢于关系2.323ms，不承诺每次重规划的端到端收益。公共层已在不含任何provider的独立构建中通过。

打钩表示本轮限定实现/因果筛查完成，**不表示DML严格零退化、通用AUTO或生产门通过**。下一步只有得到生产负载、样本量与资源窗口等输入后，才按§22的原契约开展正式验收；当前不继续重复短窗追数，也不改WAL来消除宿主I/O波动。

## 24. 八项内核质量改进并行任务

用户授权实施代码审查发现的八项问题。基线为 PG `e04022675d1235d77b8fe71a93af07396574b9ba`；保护 AGE、graph_adj 磁盘格式、原 ORCA/Daft 环境及用户数据。以下是实施任务，不是生产准入承诺；代码完成且验收通过后才打钩。

| 完成 | 任务 | 实施范围 | 验收要求 |
|---|---|---|---|
| [x] | T1 资源预算 | 消除正式路线隐藏的累计 10 万条上限；明确 work_mem 材料化预算 | 超过旧上限仍结果正确；低内存明确报资源错误，不截断、不输出后换计划；无 spill 的限制如实保留 |
| [x] | T2 端点哈希与生命周期 | 动态哈希；集合模式仅保留相邻两层；bag 历史不丢弃 | 大 frontier、高汇聚、深链、重扫及取消安全；不能把 bag 改成集合 |
| [x] | T3 跨变量条件提前执行 | 在全部操作数已绑定的最早安全阶段检查条件 | 与 relational 的完整多重集相等；NULL、边属性及历史条件不误删路径 |
| [x] | T4 访问能力与代价一致 | 批量/单键、索引方向和覆盖能力共用判断 | 不给实际不能使用的批访问或免回表打折；物理统计须匹配索引身份 |
| [x] | T5 按代价选择索引 | 替换最小 OID/首个合格索引；复用 PG IndexOptInfo/cost_index | 创建顺序不决定优选；单键、复合和覆盖索引选择有可解释成本依据 |
| [x] | T6 属性读取复用 | 查询内有界缓存已可见行的必要属性，复用已有扫描 | 相同快照、权限和重扫语义不变；宽属性不无界占用；重复输出数量不变 |
| [x] | T7 原生缓存子计划 | 共用 PG Memoize/Materialize 准入及成本逻辑 | 真实缓存子节点；参数变化、NULL、volatile、开关与 bag 正确 |
| [x] | T8 模块和元数据 | 规划、执行、诊断分层；Fixed/Native计划数据显式版本和类型校验 | copy/读写/预备计划/失效安全；编译与独立分层不产生反向依赖 |

2026-10-24最终功能核对：T1～T8均完成上表实现及正确性要求，证据见[报告§21.7](../报告/图Btree组合执行M0-M6验收报告.md#217-最终源码native元数据与独立断言验收)。Native补齐独立版本封套、typed描述、阶段列/参数来源/类型与绑定时机校验；27图模块、61项Native专项及020638四组合CASSERT矩阵通过。勾选不表示性能全部无退化或商用签收，也不表示代码已经提交。

单起点端点集合保留HashAggregate的根因亦已修复：图Path小数行数与PG去重路径的规整行数不同，触发模糊成本下的行数择优；现在遵守PG原有clamp_row_est约定，不降低成本、不扩大唯一性证明。最终f4c9五例固定6轮筛查已完成：早过滤/重复属性/宽前沿执行分别快24.24×/2.09×/1.14×，但单链/非重复属性仍有配对+1.65%/+3.04%负信号，见报告§21.8～21.9；不能将历史4.71%或本轮3.04%全部归因属性缓存。原三场景最终版12轮相对relational验证另行记录，环境不合格不追加轮次追PASS。AUTO全面选优、DML严格零退化和长期生产门仍是另外的未签收项。

并行分工：T1/T2、T4/T5、T7 各一条实现线；主集成负责 T3/T6 及最后的 T8。共享文件按函数划定所有权，统一冻结源码后重编 PG、测试模块和 AGE。先做语义与生命周期回归，再做同 SQL/数据/索引的性能对照；旧性能数字不得当本轮结果。用户已取消此前的账户剩余 10% 停止限制。

质量红线：按资深数据库内核开发标准审查语义、内存生命周期、资源界限、权限/MVCC、错误清理和现有框架契合程度；每项须实现者自审及集成者独立复核，不用“专家水平”自评替代可复核证据。

## 25. 已定位开销的定向优化（2026-10-24）

依据[验收报告§25的指令归因](../报告/图Btree组合执行M0-M6验收报告.md#25-新增cpu开销归因2026-10-24)，本轮限定优化如下。性能结果见[报告§26](../报告/图Btree组合执行M0-M6验收报告.md#26-开销优化后的三项复测2026-10-24)。

- [x] 端点插入合并HASH_FIND/HASH_ENTER，使用PG的64位整数hash；保留完整键比较、动态扩展、inline单点和相邻两层回收。HASH_ENTER命中前也可能扩容，预算检查不能只放在未命中分支。
- [x] dynahash直接成为端点context的孩子，移除多余包装context；失效前先清空读者恢复指针，再hash_destroy，不改变节点地址稳定性。
- [x] 预算仍检查实际分配器字节数与开销。叶context读`mem_allocated`，有子context保留递归；父context及全部孩子均计费。不引入固定N行抽查、预算旁路或新的隐藏行数上限。
- [x] 属性扫描初始化时记录by-reference列；新身份跳过缓存准备，重复身份才做长度检查/复制。by-value立即缓存，NULL by-reference保守延迟；不同身份、Rescan、快照和错误清理边界不变。
- [x] 扩充64位/负数/极值/重复边、NULL属性和身份切换回归；通过27图/245PG/43AGE、资源/生命周期及GUC/AGE/adj专项。
- [x] 原c99→本版a93三个10万边场景固定两次启动、12轮配对计时，28800样本独立复算；另核对48份执行指令profile。

已完成的是上述实现和有限验证，**不是全场景零退化**：单链基本持平，非重复属性本轮耗时-3.15%，稀疏键仍+4.69%。稀疏剩余指令增量主要在通用hash；若采用类型专用simplehash，必须另行解决entry移动与节点稳定引用的区别并验证资源预算，当前未实现。不得为消除这部分成本撤销T1/T2的可扩展与资源安全要求。

## 26. 类型专用哈希与稳定节点块（2026-10-24）

本节接续并覆盖§25“simplehash尚未实施”的历史状态。当前实现见`graph_fixed.c`的`GgcEndpointEntry`、`ggc_endpoint_allocate()`、`factor_node()`和`endpoint_set_next()`；详细证据见[报告§27](../报告/图Btree组合执行M0-M6验收报告.md#27-端点哈希和节点分配修复2026-10-24)。

- [x] 使用PG `simplehash`的int64专用实例，完整键比较和PG整数hash，不新增私有通用hash库。
- [x] entry只保存稳定节点指针；插入/扩容可移动entry，但不得移动已交给reader的节点或adj handle。首节点保留inline，不为单链建hash/节点块。
- [x] 节点按64槽小块分配，槽跨度MAXALIGN，包括可选handle；块只分配不移动，同层统一销毁。分配粒度不是行数/前沿容量上限。
- [x] 节点块分配后计入全部预留槽、MemoryContext块与元数据开销；块内分发不再分配内存，所以无需逐节点重复预算统计。不是payload估算或每N行抽查。
- [x] 哈希自定义分配回调在释放旧数组前检查实际占用，覆盖扩容瞬间新旧数组共存的峰值；ERROR不截断结果、不透明换路线。
- [x] reader恢复指针清空后回收已消费前沿；两层滚动、逐层集合语义、完整bag历史边界不变。
- [x] 主安装图27/PG245/AGE43和资源/生命周期/GUC/AGE/adj专项通过；新增宽前沿3/8跳、完整bag、极值键、稳定引用和回收验证。
- [x] 两个实现阶段分别完成固定三例计时及48份执行器指令核对，不把两个版本混成一个成绩。最终稀疏额外指令+0.77%，上一版a93为+2.06%；索引工作不变。
- [x] 065757四组合CASSERT独立重放/构建/回归全部通过，C/B/A完整补丁包含本次源码；四组各245 PG回归，图3/23/5/27，含B的两组资源夹具通过。
- [ ] 最终版原生时延及零退化签收：065400全部场景未过环境门，受干扰数据保留；070700三段10秒预检仍有swap-in，未启动第二次计时。预检机会已用完，后续须提供合格测量窗口，不放宽规则、不以指令比例代替时延。

本轮只改C公共内核，不改B/A接口、磁盘/WAL或开关。扩大测试时另发现adj高扇出批量构建的既存页体长度错误，已在a93/1bda分别复现，单独留待存储修复，不将其隐去或混称本轮图B-tree回退。

## 27. B-tree 扫描空工作区复用（2026-10-24）

针对已确认的扫描工作区反复malloc/free触发堆尾回收问题，底层B-tree保留最多4份、每份不超过64KiB的空闲原始工作内存。池仅在backend内使用，不包含活跃scan/relation/snapshot，也不缓存图结果、邻接或可见性判断。`btbeginscan`取出后将context归入当前扫描生命周期；`btendscan`先清理全部pin、键、数组及图提示再移到TopMemoryContext；异常时活动context按PG原机制销毁，不经过缓存回调。空闲数量/字节超限直接释放。

`tupleWorkspace`只保存可复用分配地址，`currTuples/markTuples`仅在本次index-only扫描时激活，防止普通扫描继承上一次index-only状态。复用不改变mark/restore、rescan、数组键或MVCC语义，不改图预算、页格式、索引维护及成本；不依赖glibc阈值配置。该优化作用于普通B-tree，因此验收同时重测图路线与relational。24并存扫描、回滚、错误、跨页回退、不同临时context、索引重建、新快照及原生回归通过；证据、性能口径和适用边界见[验收报告§32](../报告/图Btree组合执行M0-M6验收报告.md#32-b-tree-空工作区有界复用单链执行退化修复2026-10-24)。

## 28. 原四场景恢复优化（2026-10-25，验收进行中）

本节接续§26～27，性能证据和未恢复项以[简表§2.5](../报告/主要测试场景简表.md#25-恢复优化中的最新完整同期对照2026-10-25)为准。恢复目标是原数据、SQL、索引和结果语义不变，减少已定位的重复工作；不撤回资源预算、MVCC、权限或原生索引代价模型。

| 实现 | 减少的工作 | 保留的约束 |
|---|---|---|
| `graph_fixed.c:reuse_startup_metadata` | 有顶点检查的同构阶段也能复用已验证的启动元数据 | 关系、索引、provider、方向、属性映射、角色、快照、命令号和资源所有者必须匹配；每阶段仍初始化顶点检查，不缓存可见性结果 |
| `factor_node` / `factor_grow_buckets` | 端点集合复用稳定节点的桶链，严格递增插入延迟建哈希 | 完整64位键比较；扩容检查新旧桶共存的实际峰值；稳定节点块与相邻两层回收不变。此实现替代§26的simplehash，不是另加一层缓存 |
| `consume_endpoint_key` | 同一批内相邻重复端点只排入一次顶点检查 | 仅端点集合且两项都没有稳定引用；每条物理边仍验证、计费，带引用或bag/path语义不合并 |
| `pgq_btree_lookup_cost` | 对同类型未知参数的数组等值条件只计算一次单值选择率 | 只适用于内置`eqsel`，按`scalararraysel`原运算顺序计算合并选择率；CASSERT逐次比对原算法。实际数组长度、尾批、相关性、可见性图及`cost_index`保持不变 |
| `PgqIndexCostCache` | 同一候选中，相同物理索引的等价未知键请求复用原生估价 | 最多32项、只在一次候选估价调用内存活；匹配键数、循环数、索引/表大小、VM比例、权限、原生`has_unique_index()`证明及`pg_statistic`元组版本。常量和自定义统计/AM回调不复用；每个别名单独计算唯一性证明。CASSERT对每次命中重新计算并检查选择率和成本完全相同 |
| `IndexPathOrderCache` | 同一原生索引候选枚举中，每个方向只构造一次原始排序描述 | 只在一次`create_index_paths`的单个索引迭代中存活，保留每个Path的排序用途截断及全部候选/成本；OR位图路径不复用。CASSERT逐次重算并核对原始pathkeys一致 |
| `route_configure` | 连续相同物理边映射只验证一次各方向索引准入；精确图元素用原生OID syscache定位 | 保留已有表/索引DDL锁；映射改变即重新检查；每阶段仍独立过滤、估价并选索引。AGE/adj仍执行原同构映射和别名歧义检查 |
| `pgq_ordered_program`、临时`Param` | 只复制物理顺序会改变的字段；有界数组参数一次分配 | 逻辑图元数据保持只读，候选独占hops与stage_order；CASSERT检查原逻辑图未被改变。每个参数仍有独立节点和编号 |
| `cost_qual_eval_walker`、`pull_varnos_walker` | 常量、参数等无子节点的原生叶节点直接返回 | 不改变表达式成本或关系集合；复杂节点继续递归并保留栈深度检查，同时运行PG原生回归 |
| `visit_visible_batch` / `age_visible_visit_batch` | AGE事务私有邻接每批核验作用域，代替逐端点重复核验 | 最多128个端点；同步消费且不运行用户表达式；逐点、逐边检查中断。快照、权限、事务和禁止写入条件均保留 |

AGE可见邻接接口升级为**v2**，新增可选批接口；无批接口时公共执行器仍逐键调用。PG、测试模块和AGE必须配套重编，不能混用v1/v2安装。默认AGE开关仍关闭，事务私有缓存的装载和释放规则不变；graph_adj磁盘格式仍V7，不新增WAL或索引重建要求。

估价复用特别检查并发`ANALYZE`：平均等值选择率相同不代表物理相关性相同。每次读取原生`STATRELATTINH`的元组TID与原始xmin作为版本，随即释放syscache引用；版本变化即不命中。没有新增跨查询统计缓存，也不修改PG或AGE的统计收集方式。

验证顺序为：完整结果和资源/生命周期回归 → 四组合CASSERT独立构建 → 无采样器的六次独立启动、镜像配对计时。单次火焰图、指令减少及两启动诊断只能支持定位，不能替代正式性能验收。所有负结果和环境不合格轮保留；规划与执行分开记录，不能通过改用AUTO、改变SQL或删除重复路径来掩盖劣化。

184457组合断言验证通过上述规划增量；部分唯一索引回归令相同物理边列在不同别名上具有不同唯一性证明，并与relational完整结果对照。184936隔离副本反向移除该保护后，确实触发原生选择率等价断言；随后源码/二进制逐字恢复。故意崩溃后的fast关闭超时由该自有实例immediate关闭恢复，反向验证驱动退出1的记录保留，不能冒称正常无故障退出。

001941对未修改的f1bc二进制用GDB追踪单条十跳规划，记录50次图索引估价入口、13次原生`cost_index`调用。后续改动复用这些不同批大小请求共有的单键选择率；完整成本仍要求批大小、重复次数及index-only模式相同。递归批/尾批调度及完整成本命中时不再初始化私有Relation/IndexPath；CASSERT对复用的单键输入继续重算，尚未用采样计时签收收益。

004438组合断言进一步覆盖五文件规划增量。完整估价命中先返回，临时Var/OpExpr/RestrictInfo只在需要原生计算时构造；类型、运算符、权限、统计版本和唯一性检查仍执行。已找到等价单键输入后，只对相同批形状再检查完整成本复用。重排候选直接引用只读正向hop，反向hop浅复制并独占改变的方向、输入/输出编号和所需变量集合；谓词、约束ID只读，CASSERT检查原始IR逐字段不变。图27/PG245、资源和并发统计全部通过；正式性能以005813完成后的审计为准。

005813/629d版正式四场景各六启动、26,064样本经011113复算通过；B执行−9.09%/−1.77%/+0.33%/−7.08%，B规划−3.69%/−1.44%/−2.08%/+2.61%。高汇聚规划尚未恢复，稀疏执行仍不能证明严格零退化。005200采样在图索引估价范围129个样本中观察到22个包含`lappend`、14个包含`new_tail_cell`，支持继续减少重复参数列表构造，不能直接把样本份额当成可实现加速。

011649/a7e候选仅改`graph_pgq.c`：一次候选估价保存最长128项的只读参数节点表，较短批通过原生`list_copy_head`取得独立列表；逐字段匹配参数kind/type/typmod/collation/location，保留逐位置paramid，CASSERT逐个与新构造的预期节点比较。每个ArrayExpr有独立List，后续增长不改变已估价数组长度。索引列类型复用`IndexOptInfo.indextlist`中的匹配Var，非标准形态保留catalog查找，并用断言核对。未修改原生表达式遍历器、原生成本公式、候选数或执行器。配套O2已构建，正确性/断言/性能验证尚在进行。

012110的a7e定向诊断未证明耗时恢复（诊断中高汇聚规划旧/新中位0.2275/0.24275ms，不作为正式性能表）。继续确认批量成本已知单键选择率时，仍先构造scalar RestrictInfo再被SAOP替换，属于冗余工作。012306候选将此分支直接构造SAOP；keys=1或未得到单键选择率仍保留原生scalar条件计算，CASSERT需要时独立构造参考scalar条件核对选择率。所有数组元素、批长度、原生cost_index及成本命中保护不变。

012722只采集`pg_plan_query`的原始指令：旧d4f每条2,585,771，新5da中位2,623,147（+1.45%）；新版本5个热样本为2,623,147/2,622,995交替，原始self指令和与规划入口计数均精确相等。这不是耗时比例。剩余增量包括数组参数逐元素进入`cost_qual_eval_walker`、`pull_varnos_walker`和通用walker；012907候选为ArrayExpr直接遍历，成本跳过Var/Const/Param，关系集合仅跳过Const/Param，复杂节点仍按原顺序递归且ArrayExpr仍执行栈深度检查。增加27组generic plan/返回值对照，覆盖未知参数、复杂函数成本、NULL/外连接、LATERAL、关联子计划及嵌套数组；验证中，不能预先宣称收益。

013103的27组普通SQL完整计划/成本/返回值对照全部相同，当前7a51图27/PG245/AGE43、013141组合CASSERT/资源/并发统计、013348原四场景完整结果验证全部通过。013445规划原始指令新中位2,593,227，相对旧d4f额外+0.29%，较上一候选少29,920条；只能说明CPU工作减少。013451定向诊断通过但不代替签收，013546六启动四场景正式计时进行中。迁移增量更新为七文件并通过原快照正向/当前反向检查，未重跑完整四组合。

013546/7a51正式四场景六启动计时经014809复算26064条通过：B执行−4.20%/−6.56%/+0.48%/−5.81%，B规划−2.57%/−2.46%/−1.42%/+2.69%。仍不签全部恢复。

**采样边界修正：** 001941 GDB及012722/013445 Callgrind触发硬编码20ms搜索限时，只完成首个候选；前述50/13次调用和+1.45%→+0.29%指令比例不能描述正式完整八候选。正式四例各六份B计划均候选8/预算8/未限时，全部3600条after B规划最大0.691ms，见164325下`planner-scope-audit.json`。正式耗时不受采样器影响；后续指令脚本会记录候选数及是否用尽配置预算。

015731候选新增`pgq_reverse_start_possible`：线性链任意非最左起点，都必须反向遍历第一个逻辑hop。若该hop对应的PG `IndexOptInfo`列表中没有满足原生估价必要形态的B-tree首键，该候选最终必然不能取得`index_choices`，可以在构造物理program和访问索引目录之前拒绝。仅使用后续`pgq_select_btree_index`同一列表/必要过滤条件；未知映射和无向形式继续原检查，没有缓存目录缺失结论。保持tried/budget计数和搜索限时；已有反向索引的候选照常完整估价。CASSERT仍执行被提前排除候选的原路径，并断言普通/独立distinct路径均没有新增。验证中，不提前发布收益。

021004/b37正式六启动四场景经022255复算26064条通过：B执行−4.07%/−3.86%/+0.38%/−4.82%，规划−3.13%/−3.49%/−2.58%/+0.42%；仍未签严格无劣化。020907读取实际IndexOptInfo确认反向预检false，搜索计数8/8无限时；020751仅GDB优化变量不可读，原失败保留。

022339/0e7a71c9候选针对`join_var_pair`的多余目录访问：原流程对每跳每条条件先查询整数等号操作符族，最后才比较变量和列。现先匹配rti/attnum的正反变量对，匹配后仍完整检查整数类型、等号操作符族及既有collation/SRF边界。仅交换必要合取条件顺序，没有缓存、成本公式或语义变化。首候选采样中该函数171012条Ir，其中操作符族查询126400条，说明优化依据但不是完整八候选耗时；待实测。另加强CASSERT排除候选检查，保存List元素指针副本并逐项比较，避免原地修改List时只比地址漏检。图27/PG245/AGE43已通过，组合断言和进一步验证进行中。

022521组合CASSERT/资源/并发统计、022729旧10月基线对照和022820即时b37对照的原四场景完整结果全部通过。022903以b37为直接对照，五个热样本均完成候选8/预算8/未限时（首个预热限时且不计），规划Ir中位2,539,396→2,406,503，减少5.233%；`join_var_pair→get_opfamily_member`子树126,400→6,320。原始self和等于总Ir，核对在164325的`qual-order-instruction-audit.json`。不同于更早采样，本次热样本完成当前全部预算位置，其中不可能的反向候选正常预检排除；不能将指令比直接称为耗时比。023032无采样正式计时开始。

023032/0e7a经024317复算26064样本通过：图B-tree四场景规划−9.58%/−8.35%/−5.53%/−3.91%，各探索区间上界低于零；单链/连续/平行执行−7.97%/−5.47%/−5.75%，稀疏执行仍+1.21%（−3.58%～+7.16%）。规划差额闭环，稀疏执行继续定位。024318执行Ir较旧多23487（+0.240%），不能直接解释1.21%耗时。

024808/cfd8101d仅在空前沿跳过必败的factor_find_node；非空前沿按原逻辑查重，物理边收费/验证和取消检查照旧。同时read_direct_rows将同步调用内不变的本跳类型、opposite列、容量及阶段指针保留为局部量，避免逐行重复查找；父端点rescan只改key，逐读MVCC/租约/内存预算全部保留。没有复用邻接/可见性结果。图27/PG245/AGE43、024907完整结果、025036批访问、025044覆盖索引通过；025054组合断言已通过图27/PG245、资源及360次并发统计规划。025024原五个热执行指令均旧9789401→新9775057，较旧−0.147%，较上版少37831条；核对见164325的row-invariants-instruction-audit.json。正式耗时待测。


### 28.6 最新耗时验收与同版本控制（2026-10-25）

024808/cfd的正确性与组合断言验证完成。025559完整六启动计时及030833复算通过原始数据校验，但单链/平行边未满足双角色环境门；连续B执行−4.77%，稀疏+3.26%（−9.54%～+5.88%），不能签全恢复。030928同版本稀疏A/A六启动合格、031250复算7416条通过，启动差额−2.37%～+4.87%，未证实固定角色偏差；不能通过减去控制组差额修饰结果。

031301阶段采样未发现稳定新增执行开销，主要热点仍原生B-tree比较/缓冲访问；本轮没有继续盲改内核。正式表和验收边界见[主要测试场景简表§2.5](../报告/主要测试场景简表.md#25-恢复优化中的最新同期对照与验收边界2026-10-25)。后续须有安静测量条件或新的可检验根因，不回退MVCC/资源契约，不缩减候选预算。未完成项和机器证据见[最新诊断记录](../执行包/evidence/runs/20261024T164325.020782Z-G1-080-report-stage-reuse-optimization/row-invariants-latency-diagnosis.json)。

### 28.7 同一持锁内部页复用有序键的定位下界

2026-10-25新增优化位于`nbtsearch.c::_bt_graph_routes()`。旧实现已经批量路由，但对同一个读锁保护的内部页，仍为每个有序键重新校验跨锁提示。这里的上一次定位实际上没有失效：页保持读锁，且原生数组预处理已经保证准入的单列升序bigint键有序。

外层将已经找到的pivot传入；当前键直接复用该位置，后续更大键从其右侧指数探测，再在缩小的区间中二分。比较仍使用原`_bt_compare`，保留pivot截断和父页high key规则。这个精确下界只在本次持锁调用内成立；跨锁的提示仍重新校验，缓存的叶页编号仍需普通pin、锁和右移修正。没有缓存查询结果，也不改变MVCC、权限、SSI、预算或候选数。

CASSERT逐个路由键与原`_bt_binsrch`对照。033214组合断言通过图27/PG245、资源及360次并发统计规划；033703专项用100,005条边、正负稀疏键和int64两端极值，核实两个索引均为根层级2，70项检查覆盖批次1/7/32/128、路由/直接比较/seek开关、删除/VACUUM、REINDEX和缓存计划，并核对bag重复次数。Release的图27/PG245/AGE43、原四例完整结果、批访问和覆盖索引专项同样通过。

033138与033902原始指令复算表明，稀疏查询相对此次修改前cfd版减少263,612条执行指令（−2.697%），相对10月6日源码重建版减少2.839%；叶页二分等其余主要读取工作保持相同。这是工作量证据，耗时由033731固定六启动无采样对照另验。当前O2构建032844/da987457，新增文件属于B层；现有八文件C层增量保持独立，不把本次B层改动混入公共层补丁。组合断言通过不等于已重跑四组合迁移。

实施、原始采样和迁移增量见[持锁路由记录](../执行包/evidence/runs/20261025T032750.775657Z-G1-080-locked-page-route-optimization/progress.json)及[指令复算](../执行包/evidence/runs/20261025T032750.775657Z-G1-080-locked-page-route-optimization/locked-route-instruction-audit.json)。

### 28.8 已准入 bigint 图扫描的叶页比较

033731配对计时合格后，稀疏键仍有+0.64%的不利点估计。采样中`_bt_compare`占执行self指令12.61%，其中叶页二分反复经过通用比较入口。040045/19379fe5进一步在`_bt_binsrch`中识别既有`graphint8`准入，并使用叶页内的固定类型比较循环；搜索值只取一次。`_bt_compare_int8_tuple`复用原来的bigint元组读取规则，供通用入口和叶页循环共用。没有改变二分次数、数据访问或索引页格式。

准入仍由`_bt_first`证明：内置int8比较、索引只有一个属性、正向升序、非NULL搜索键、无scan TID、非parallel、`nextkey=false`。普通扫描保持原二分循环；内部pivot仍走原比较；NULL索引元组调用原生比较处理NULL排序，不依据表的NOT NULL约束假定所有物理索引项非NULL。

CASSERT每次快速叶页查找都构造独立参照键，把`graphint8`关闭，再用原生二分确认返回位置；只复制实际的一个扫描键，避免对变长分配执行整个结构体拷贝。逐项元组结构断言、pin/锁、可见性、SSI和预算保留。该专用小循环与原生实现之间的对照，也用于发现未来原生比较语义变动。

040152图27、040205 PG245、040223 AGE43、040229批访问、040238覆盖、040245原四例完整返回值通过。040327组合CASSERT/资源/360次并发统计规划，以及040538的134项多层索引专项通过；后者增加已删除NULL残留项，分别使用NULLS FIRST/LAST，覆盖1/7/32/128批次和路由/seek/快速比较开关。

040541与040644原始40份热dump复算通过。相对旧源码重建基线，单链/连续/稀疏/平行图B-tree执行指令减少11.64%/0.72%/9.83%/9.10%；稀疏为9,790,060→8,827,696条。该证据证明工作量减少，实际耗时由040644独立固定六启动无采样对照验收，不从指令直接折算倍率。设计、当前进度、冻结的前一版和分层补丁见[叶页优化记录](../执行包/evidence/runs/20261025T035251.035191Z-G1-080-leaf-compare-optimization/progress.json)。

040644无采样对照及041923复算已完成：四例图B-tree自身执行−5.13%/−5.30%/−5.85%/−8.28%，全部满足环境门且探索区间上界低于零；限定基准自身执行恢复完成。平行边相对relational倍率仍降低，因为relational自身改善更多。当前表已更新19379，具体边界见简表§2.5；其他生产/迁移/严格零退化阶段门不变。


## 29. 多跳审查修复后的执行与统计契约（2026-10-25）

本轮实现与证据见[验收报告§35](../报告/图Btree组合执行M0-M6验收报告.md#35-重新审查后的修复与验收2026-10-25)。最终065407/49827567，图30、PG245、AGE43及专项通过。以下是设计约束，不是生产准入承诺。

1. 顶点VM免回表遵循原生index-only pin契约：rescan前设置`xs_want_itup`，叶页pin保持到VM/可见性判定后；统一单键/数组/范围的扫描创建、重扫及释放。必要heap回退、SSI、快照和权限仍保留。旧版确定性交错3种返回不存在的顶点路径，修复后六种交错全部正确。
2. 代价按边过滤→顶点检查/过滤→具备依赖的跨变量条件→下一层传递。B/P为路径数，F/E/A为去重工作，N为批请求；READY样本不能把剩10条平行路径对应的1个端点估成0.1。重复变量/严格等价常量条件只计一次选择率，保留执行recheck和CPU计费。相关性和度数分布未知仍用保守估计，不假定精确。
3. 统计按本角色作用域授权，要求图及全部基表完整SELECT与维护权限，不共享管理员可见总体。变更计数基于PG既有pgstat及本backend pending，不新增逐条DML hook。鲜度不充分或变化则实际回退；其他backend异步flush、统计快照、回滚保守失效和VACUUM/冷热未全跟踪的边界保留。raw统计读写仍限超级用户。
4. 所有图统计消费者（含logical-only/MISSING）携带图统计目录依赖。角色/成员资格变化仅使这些cached plans失效，沿用PG已有计划失效扫描，不广播新sinval、不重置无关普通计划。首次COST创建/发布、表DDL及权限变化也遵循依赖失效；普通DML不强制generic重规划。
5. Fixed按真实首批和前缀物化计算startup。仅forced B-tree、AGE关闭且当前root部分读取时，追加已有Native流式候选并保留Fixed，由PG选择；无关系DP、无运行期切路、无新增缓存。LIMIT1可避免先物化完整前沿，但外层Sort/Agg及Native内部TOTAL贪心的限制仍在。未增加spill。
6. 大父批排序分组并按输入顺序恢复request，保留完整句柄等价及父身份，排序临时空间计预算，比较及循环有取消检查。预算分别说明统计对象，不把逻辑容量当backend实际总内存。小批保持轻量路径。
7. 公共text数组构造和顶点扫描寿命已合并。batch复制行、B-tree lease及AGE回调有不同拥有/借用边界，完整边游标统一应独立设计；不为代码行数混淆pin或结果寿命。

最终时延尚未签收：固定四场景结果一致但换页使计时门不足；旧19379恢复倍率不能直接当4982结果。下一轮应先取得合格环境，保持相同SQL/数据/计划路由和完整结果校验。


### 29.1 最终性能验证与提交补充

最终4982实现已按公共层7feb54db、B-tree539cf867、adjca3c0206及AGEa1cf53a6提交，源码/二进制字节不变，未重新编译。四原场景中单链/稀疏/汇聚计时合格、连续键端口角色分布不合格。三跳4096分支LIMIT1/10/100原Fixed→Native纯执行配对89.79/55.33/13.40×，相对relational仅1.06/1.24/1.08×；证明避免完整前缀物化，不代表普遍跨系统加速。详见[报告§36](../报告/图Btree组合执行M0-M6验收报告.md#36-审查修复后的最终性能复测与分层提交2026-10-25)。两个小幅不利时延信号与严格零退化边界保留，未为计时噪声修改设计契约。
