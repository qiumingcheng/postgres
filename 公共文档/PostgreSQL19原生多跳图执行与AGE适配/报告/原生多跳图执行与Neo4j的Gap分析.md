# 原生多跳图执行与 Neo4j 的 Gap 分析

## 0. 当前结论：整合“检查图btree实现不足”会话（2026-10-23）

本节优先于下方历史快照。来源会话为 **检查图btree实现不足**，ID `01a168e7-8602-76c2-9de9-b0706345b21e`；已读取其中功能对齐、跳数限制、变长路径及 VLE 的全部用户问答。原文时间为北京时间 2026-10-23 21:24～21:51，首问是“帮我看一下，当前图btree的实现还有哪些不足？”。只整合图相关结论，不复制无关环境信息。

复核对象是 PG 工作树 `.worktrees/pg19-graph-core/postgres19` 的干净 HEAD `e04022675d1235d77b8fe71a93af07396574b9ba`，与原会话核查版本相同；AGE 为 `01f2d3694235baefb21d66acb6634404744d5782`。本次核对源码、既有验收和相关官方文档，**没有修改内核、重新编译或重跑测试**，不是一次全面代码安全审计或 Neo4j 性能 PK。

### 0.1 总判断与已接通范围

**原会话的核心判断成立，没有发现其四类主要功能缺口与当前实现相悖。准确定位是：固定长度属性图查询及 PG 集成已有较完整的限定覆盖，但尚未与通用图查询能力全面对齐。** “已有覆盖”不能理解成所有组合、所有 FDW 或生产环境均已验收。

必须区分同一个 `GraphRoute(btree)` 下的两种执行路线：

| 路线 | 当前实现 | 不能据此推导的结论 |
|---|---|---|
| `GraphExpandFixed` | 整数键公共多跳执行器＋普通 B-tree provider；合格场景有批访问、覆盖索引和安全的逐层端点集合优化 | 不能将去重用于任意 bag、完整路径或历史相关过滤；也不代表所有整数查询都满足快速路径准入 |
| `GraphExpandNative` | 通用类型的逐阶段绑定状态机，复用 PG 的参数化扫描子计划；支持已验收的复合键、NULL 端点、RLS、分区/继承、视图/file_fdw、LATERAL、多标签分支、合格部分/表达式索引及复杂表达式 | 不是把整图关系 JOIN Plan 改名；但也不保证每个孩子都是 B-tree 扫描、不继承快速路线的批访问和去重收益；目前只在强制 B-tree 路线进入 |

通用路线源码：[graph_pgq_native.inc](../../../.worktrees/pg19-graph-core/postgres19/src/backend/executor/graph_pgq_native.inc)、[正式路由与回退到通用执行](../../../.worktrees/pg19-graph-core/postgres19/src/backend/executor/graph_pgq.c)。两轮 Top 3 和九类接入的范围、正负例见[验收报告 §14～§16](图Btree组合执行M0-M6验收报告.md#16-九类原生能力接入2026-10-23)。

因此，下方历史表里的“无向/IN 不支持”“只支持 bigint 和同构表”“RLS、复合键、分区、LATERAL、一般属性尚未接通”等，**不能继续作为当前缺口**。当前已覆盖 OUT/IN/无向及混合方向、异构固定路径、重复顶点变量闭环/自环、点边属性输出及过滤、外围 PG 算子组合；仍保留各路线的准入条件，不能据此扩大 AGE/adj 的范围。

### 0.2 原会话各项判断的复核与修正

| 原会话结论 | 复核结果 | 需要明确的边界和代码依据 |
|---|---|---|
| 变长路径尚未接入 | 成立 | `transformGraphElementPattern()` 遇到非空 `quantifier` 直接报错。语法规则能构造 `{n}`、`{m,n}`，不等于分析阶段允许执行；**关系路线也会在同一解析层被拒绝**。[解析检查](../../../.worktrees/pg19-graph-core/postgres19/src/backend/parser/parse_graphtable.c:243)、[量词规则](../../../.worktrees/pg19-graph-core/postgres19/src/backend/parser/gram.y:18346) |
| 单个 GRAPH_TABLE 内多个路径模式尚未接入 | 成立 | `transformPathPatternList()` 要求路径模式列表长度为 1。这不是“数据不能有分叉/汇聚”，也不是“多个 GRAPH_TABLE 不能用 SQL JOIN 组合”。[解析检查](../../../.worktrees/pg19-graph-core/postgres19/src/backend/parser/parse_graphtable.c:340) |
| 最短路、路径对象尚未完整接通 | 成立，须区分能力层次 | 当前图语法没有对应路径选择器和路径变量入口；固定位置的 ID/属性输出、内部保存路径绑定，不等于可返回通用路径值并调用路径函数。能手写递归 SQL 或拼数组，也不等于已有图最短路/路径对象。[图语法](../../../.worktrees/pg19-graph-core/postgres19/src/backend/parser/gram.y:18172) |
| 正式入口为 WALK，TRAIL/ACYCLIC 等未接通 | 成立 | `build_pgq_fixed_program()` 明确设置 `constraint->mode = GGC_WALK`；`graphnodes.h` 中有枚举和测试构造，不是正式查询支持。固定路径可重用同一顶点，和“禁止重复顶点”不是一回事。[重写](../../../.worktrees/pg19-graph-core/postgres19/src/backend/rewrite/rewriteGraphTable.c:802)、[快路径准入](../../../.worktrees/pg19-graph-core/postgres19/src/backend/executor/graph_pgq.c:985) |
| 256 跳已有功能验收 | 成立，但属于历史功能证据 | 2026-10-21 专项 `256-hop-budget` 为 PASS、一个图节点、输出 1 行；不是本轮重测，不代表所有 256 跳模式/数据都可承受，也不是最大跳数证明。[原始证据](../执行包/evidence/runs/20261021T103530.735379Z-G1-080-btree-orders/summary.json) |
| 整数执行器有 32,767 跳保护阈值 | 成立，不能说“支持到 32,767 跳” | `validate_options()` 在 `stage_count > PG_INT16_MAX` 时拒绝；前面的解析、规划、内存和其他预算可能先触发限制。这是 `GraphExpandFixed` 的检查，不能泛化成所有路线共同的可运行上限。[检查](../../../.worktrees/pg19-graph-core/postgres19/src/backend/executor/graph_fixed.c:537) |
| 路径统计支持 2～4 跳，不限制查询只能四跳 | 成立，需限定为专用采集器及相应条件估计 | `PATH_MIN_HOPS=2`、`PATH_MAX_HOPS=4`；正式条件基数消费也检查这一范围。较长固定查询仍可用其他估计/回退，不是所有 PG/图统计都只能用于四跳。[采集](../../../.worktrees/pg19-graph-core/postgres19/src/backend/statistics/graph_stats_path_collect.c:43)、[消费](../../../.worktrees/pg19-graph-core/postgres19/src/backend/executor/graph_pgq.c:1642) |
| 新通用路线需强制 hint，AUTO 未全面开放 | 成立 | 默认 `enable_graph_btree=false`；通用 `native_access` 和外围 JOIN 区域使自动图接管退出。限定整数 AUTO 已有修复，不能说完全没有 AUTO，也不能说通用 AUTO 已完成。[路由](../../../.worktrees/pg19-graph-core/postgres19/src/backend/executor/graph_route_hint.inc:220)、[默认值](../../../.worktrees/pg19-graph-core/postgres19/src/backend/optimizer/path/graphpath.c:18) |
| AGE 已有 VLE，但我们的 PGQ 图 B-tree 没有接入 | 成立 | AGE 的 `age_vle()` 与事务私有 `age_visible_adj` 是两套入口。启用“公共执行器＋AGE 内存邻接”不会自动把 PGQ 变成 VLE。[AGE VLE](../../../.worktrees/pg19-graph-core/apache-age-pg19-port/src/backend/utils/adt/age_vle.c:1932)、[邻接适配](../../../.worktrees/pg19-graph-core/apache-age-pg19-port/src/backend/utils/adt/age_visible_adj.inc) |

### 0.3 VLE 的直白解释及不能混淆的语义

```text
张三 → 李四 → 王五 → 赵六
```

固定两跳只匹配到王五；允许 1～3 跳，则李四、王五、赵六都属于可能的终点。AGE 的示意写法是：

```cypher
(a)-[*1..3]->(b)
```

这是 **AGE/Cypher 示例，不是当前 PGQ 图 B-tree 可直接执行的 SQL**。VLE 在 AGE 文档中称为 Variable Length Edges；它覆盖合法长度范围，并不要求为每种长度从头重跑一次。[AGE 官方说明](https://age.apache.org/age-manual/master/clauses/match.html#variable-length-edges)

原会话“区间所有跳数都尝试”作为语义解释没错，但要补充三点：

1. **VLE 不等于终点 DISTINCT。** 同一终点可以由不同路径、不同长度到达；是否只留一行由查询语义决定。
2. **当前逐层去重不等于跨层全局 visited。** 固定 exact-k WALK 允许一个点出现在不同深度，不能为扩展 VLE 直接改成“见过就永不访问”。内部契约已明确区分。[接口契约](../../../.worktrees/pg19-graph-core/postgres19/src/include/executor/graph_fixed.h:176)
3. **无上界 WALK 的全部路径不一定有限。** 有环并允许重复边时，可构成无限多条路径；“所有可达点”与“所有路径”必须分别定义终止、重复规则和资源预算。不能只删除跳数上限就宣称实现 VLE。

分别查询固定 1/2/3 跳再合并，是有界范围的表达办法，不是原生变长算子。需要保留路径次数时不能随意用 `UNION` 去重；要终点集合时也要显式确定去重范围，并对齐边/点重复规则。

### 0.4 业界对标需修正的表述

- Oracle 26.1 SQL/PGQ 官方文档支持**有界递归路径与多路径模式**，原会话这句话成立；同一文档仍列出路径变量、`ALL SHORTEST` 等匹配目标不支持。不能将它概括成“Oracle 的 SQL/PGQ 已支持上述所有缺口”，也不能把独立图算法函数和这些语法入口混为一项。[Oracle 功能与限制](https://docs.oracle.com/en/database/oracle/property-graph/26.1/spgdg/supported-feature-and-limitations-when-querying-sql-property-graph.html)
- Neo4j Cypher 提供变长、非线性模式、路径变量和按跳数的最短路；**加权最短路**在该官方指引中归入 GDS 选择，不应算作普通 Cypher 多跳同一项能力。功能对标成立，不代表已经测出双方性能差额。[模式](https://neo4j.com/docs/cypher-manual/current/patterns/)、[路径变量](https://neo4j.com/docs/cypher-manual/current/patterns/fixed-length-paths/)、[最短路与 GDS 边界](https://neo4j.com/docs/cypher-manual/current/patterns/shortest-paths/)

### 0.5 整合后的后续方向（建议，不代表已授权开发）

- [x] 读取指定会话，复核图相关结论，区分快路径/通用路线及原生前端限制，纠正旧缺口状态。
- [ ] **先做有界变长。** 同时明确 WALK/边不重复/点不重复、bag/终点集合和路径输出契约；贯通语法、图 IR、规划、执行、预算及回归，不能仅取消解析报错。当前固定快路径保留。
- [ ] **再做单 MATCH 多路径模式。** 明确共享变量、闭合/交汇约束、路径次数及联合规划；多个 SQL 子查询能 JOIN 不作为此项已完成的证明。
- [ ] **补最短路与路径值。** 路径值的基础表示随变长设计提前确定；无权最短、全部最短与加权最短分开验收，不把最后一项默认塞进前两项。

这延续原会话“有界变长 → 多路径与重复规则 → 最短路及路径对象”的方向，但**重复规则和路径值的接口设计应前置**，不是等算法完成后才补语义。它们与当前图 B-tree 架构不冲突，却需要新增正式前端和执行能力，不是打开一个现有开关。

AUTO 扩围、快速路线合格边索引的成本选优，以及 DML/生产验收仍是独立事项：快速路线的 `route_choose_index()` 目前仍按合格索引最小 OID 选择；通用路线已复用 PG 扫描成本选择，不能把这两者混写。详见[索引选择](../../../.worktrees/pg19-graph-core/postgres19/src/backend/executor/graph_route_hint.inc:268)及[当前验收报告](图Btree组合执行M0-M6验收报告.md)。本次不修改默认开关、正式任务状态或既有性能数字，不把功能接通当作生产门签收。

---

**以下为历史审查快照。§1～§7 中的“当前”“未支持”和旧源码行号均指当时版本，不是 e040226 的现状；当前判断统一使用上方 §0。** 原始差距编号和当时证据保留，便于追溯，不再据此重复立项已完成的功能。

> **2026-10-23第二轮接入更新：** 图B-tree现支持smallint/integer/bigint单列非空键及跨整数连接、必要列的列级SELECT授权、同边表不同图元素/端点映射。I01不再是“仅bigint”，但text/UUID/复合键、可空端点仍未支持；RLS、多标签`f|r`的UNION、分区/LATERAL等边界没有因此关闭。图21/PG245/AGE43、新专项30等通过，见[验收§15](图Btree组合执行M0-M6验收报告.md#15-第二轮-top-3-接入2026-10-23)。未重新调研Neo4j或做性能PK，下方是历史审查快照。

> **2026-10-22 Top 3接入更新，优先于以下历史快照：** 图B-tree已接通异构点/边表固定路径、非首列bigint单列键/三列边表/合格UNIQUE索引，以及重复顶点变量闭环、自环。旧表G07的“异构/闭环不支持”和I02～I04的列布局/特定主键限制已在此限定范围内补齐；I01的文本/复合键限制仍在，任意多路径、变长/shortest、RLS/分区/LATERAL等不因此支持。图20、PG245、AGE43及新两会话专项13通过，见[本轮验收§14](图Btree组合执行M0-M6验收报告.md#14-pgq-top-3-接入补齐2026-10-22)。本轮没有重新调研Neo4j或做性能PK。

> **2026-10-21 后续实现更新：下文是原提交的差距审查快照，不再是当前能力清单。** M0～M5已经补齐受限PG算子组合、typed属性/历史过滤、IN/OUT混合及无向bag、多区域强制组合和有界起点/顺序枚举；M6将入口迁入内核，不再依赖LOAD测试模块。仍非Neo4j全能力：异构/任意分支/变长/shortest、图内部RLS/分区/LATERAL等未由本轮交付；AUTO默认关闭，外围JOIN保留原生规划，代价准确性仍有缺口。当前证据与边界以[组合执行设计](../设计/图Btree与关系算子组合执行设计.md)为准，本文原源码行号仅适用于审查时版本。Neo4j一侧本轮未重新调研，也未进行双方PK。

核查日期：2026-10-21。

本文对照当前源码和 Neo4j 官方源码、文档，分析多跳查询从语法到计划、执行的能力差距。Neo4j 对照仍为源码与官方资料审查，不是双方性能 PK。2026-10-21 后续已实施并验证本地 M0/M1 组合增量，相关更新见 §3.1、§4 的 I09 及[组合执行设计第7～8节](../设计/图Btree与关系算子组合执行设计.md)；其余差距不能据此当成已关闭。

## 1. 结论先行

> 历史快照；当前判断以 §0 为准。

**当前实现是一个已经有阶段性能证据、但准入范围较窄的固定多跳加速器，尚不是与 Neo4j 同等覆盖范围的通用图查询优化器。** 差距不只有“少枚举几个起点”。

当前标准 PGQ 专用路线的典型范围是：同一顶点表、同一边映射、固定 N 跳、按写法从左到右沿 OUT 展开、WALK 语义、单列 bigint 键，并满足指定的物理列映射、权限和表类型要求。中间点/一般边过滤、普通顶点属性输出等常见业务需求，仍可能使查询无法进入该路线。依据：[PGQ 准入检查][P-contract]、[hint 配置][P-route-config]。

**专用路线不能接入，不等于 PG19 不能执行。** 原生 PG19 已支持无向匹配、点边属性过滤、异构路径、闭环、分区及 RLS 等能力，通常重写成关系查询执行；当前基线也确有量词路径、多路径模式等原生限制。§4 用同一张表区分这两类问题与已有优化的覆盖边界。

特别需要明确的结论：

- **无向模式 `a-[e]-b` 当前不能进入我们的标准图专用路线。** 不是“支持但未优化”。
- **左向模式 `a<-[e]-b` 和含 IN 阶段的混合方向模式，也未接通当前标准 PGQ 专用入口。** 底层 OUT/IN provider 接口不能作为前端已支持的证明。
- 索引成本选优、属性谓词、复杂图模式、变长/最短路径、较广的安全剪枝、自然选路等，都还有独立工作。
- 另有列位置、至少四列边表、分区父表、RLS、关联外部输入、hint 覆盖范围等 PG 集成限制，不能藏在“支持图查询”的笼统描述里。
- **不能据这些差距推导 Neo4j 一定快多少；也不能用我们相对 relational 的倍率证明已经接近或超过 Neo4j。**

## 2. 核查对象与判断口径

### 2.1 固定版本与范围

| 对象 | 核查版本 / 位置 |
|---|---|
| PG 图开发树 | `/home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19` |
| PG HEAD | `5d3d3c1d29af20946b238a4bbde2c38bebeefd77`；公共层 `c8166b1e`，B-tree 层 `9b260b0e`，adj 层 `5d3d3c1d` |
| PG 原始基线 | `7873db5369b967dd53984df34fd800003410d04f` |
| AGE 适配树 | `.worktrees/pg19-graph-core/apache-age-pg19-port`，`01f2d3694235baefb21d66acb6634404744d5782` |
| Neo4j 规划器源码 | 官方仓库 `2026.09` 引用；文中链接固定在该引用 |
| Neo4j 产品能力说明 | 官方 current 手册，本次查询日期如上；涉及 Enterprise/Aura、Cypher 25 的能力单独标明 |
| 本文比较范围 | 多跳模式、规划选优、相关执行优化及接入边界；不覆盖 Neo4j 全部数据库、集群、管理和 GDS 能力 |

最初核查时两份源码树均为 clean；后续 PG 有未提交的 M0/M1 增量，HEAD 不变，AGE 仍 clean。当前代码属于本地开发分支，不代表上游 PostgreSQL 已包含我们的执行器。

已有性能证据继续以 [GraphRoute 三路 hint 使用与性能](../执行包/golden/G1-080/GraphRoute三路hint使用与性能.md) §2.19～§2.20 为准；本报告不替换或补造其中的测试数字。

### 2.2 必须区分四个层次

| 层次 | 能证明什么 | 不能据此证明什么 |
|---|---|---|
| PG 原生语法/关系重写 | 该 SQL 可以被解析、重写为关系查询 | 一定进入 `GraphExpandFixed` |
| `GgcFixedProgram` 中间表示 | 能保留某种方向、变量和映射 | Path 准入会接受，或者存在相应运行时算法 |
| 公共执行器/provider | 某个接口或底层测试能执行 OUT/IN、批访问等 | 标准 PGQ 用户入口已连接该接口 |
| 标准 PGQ 计划和运行时 | 用户 SQL 真正选中专用计划，且相关优化已启用 | 所有语义、数据形态和生产环境均已验收 |

此前只写“当前全 OUT”而没有单列无向模式，是报告表达不充分。更深的原因是没有将“底层能力”和“用户端到端可用能力”逐项列成矩阵。后续能力声明应以最后一层为准。

## 3. 方向能力：无向和 IN 必须明确列出

这里的“无向”指查询忽略边的存储方向，不是把一条物理边改成另一种存储实体。Neo4j 的 `-[r]-` 支持任意方向匹配，`-[r]->`、`<-[r]-` 则约束方向。[Neo4j 固定路径说明][N-fixed]

以下只讨论方向这一项，其余键、表、输出等准入条件仍须满足。

| 查询模式 | 本地 PG 原生关系重写 | 我们的图 IR | 当前标准 PGQ 专用 Path | 底层接口状态 |
|---|---|---|---|---|
| `(a)-[e]->(b)` | 有对应逻辑 | `GGC_OUT` | 可准入 | 支持 OUT |
| `(a)<-[e]-(b)` | 有对应逻辑 | 可表示 `GGC_IN` | **拒绝 IN 阶段** | 公共执行器、B-tree provider 有 IN 接口 |
| `(a)-[e]-(b)` | 有匹配两种方向的关系条件构造 | **遇到 `EDGE_PATTERN_ANY` 返回 NULL** | **不能进入** | 没有接通该模式所需的组合访问与语义处理 |
| 多跳中混合 `->`、`<-` | 可以构造相应关系条件 | 可表达各个 OUT/IN 阶段 | **只要含 IN 阶段就不符合当前准入** | 不能从单个方向接口反推前端支持 |

三处直接证据：

```c
/* rewriteGraphTable.c / build_pgq_fixed_program() */
if (pf->kind == EDGE_PATTERN_ANY)
    return NULL;  /* 不生成我们的 graph_program */

/* test_graph_fixed.c / pgq_program_contract() 的拒绝条件之一 */
hop->direction != GGC_OUT

/* graph_btree_provider.c / btree_request_valid() */
(r->direction != GGC_OUT && r->direction != GGC_IN)
```

依据：[IR 构造][P-lowering]、[PGQ 准入][P-contract]、[B-tree provider][P-provider]。PG 原生对无向模式构造正反两组连接条件的代码在 [generate_query_for_graph_path()][P-native-direction]。

还要区分两件事：

- `a<-[e]-b` 是用户模式里的方向。
- 对 `a-[e]->b` 选择从右端开始，沿 IN 执行，是优化器的执行顺序选择；不改变查询中边的方向语义。

当前既不能把标准入口的 IN 模式算成已接通，也没有不同起点的通用枚举。

### 3.1 关系 hint 的额外限制已修复

最初发现 `route_planner()` 连 relational 都要求一个图程序。M0 已将此条件移入专用图分支：

```c
if (mode != ROUTE_RELATIONAL)
{
    route_find_program((Node *) parse, &search);
    if (search.count != 1)
        route_error("Exactly one supported GRAPH_TABLE is required.");
    /* 后续配置和校验图路线 */
}
```

因此，简单无向和 IN 等原生支持的查询可以显式使用 `GraphRoute(relational)`，不再被专用图资格挡住；**btree/adj 专用入口的方向限制并没有因此解除**。依据：[hint 规划入口][P-route-planner]、[新增回归](../执行包/evidence/runs/20261021T084052.908944Z-G1-080-graphroute-recovery-modules/summary.json)。

本修复与 M1 已在当前未提交增量中完成，不再是待实施建议。M1 仅打通单个合格图区域和外围 PG 算子的受限组合；不等于图内部参数化、方向扩展或自然选优已完成。

## 4. 统一对照表：原生 PG19、我们的专用路线与 Neo4j

> 历史快照；其中未接通项不能直接当作当前待办，现状见 §0。

本节合并原第 4、5、6 节，按能力去除重复说明。G 为图语义/规划，I 为 PG 接入，O 为执行优化边界；保留原 G/I 编号便于追溯，补充 G13/G14 和 O 编号。**“我们”仅指当前标准 PGQ 专用路线，不代表整个 PostgreSQL 的能力。**

PG19 列限定为 §2 的本地基线：支持图查询通常指重写为关系查询执行，通用 SQL 能力会明确标注；不把普通 JOIN/递归算法当作已经实现同名图算子。原生解析器和图回归文件与基线无差异。Neo4j 没有直接对应的 PG 集成项标为“不直接对应”，未核验的优化不默认算支持。M0/M1 新测范围以链接证据为准，其余仍为源码/官方资料结论。

| 编号 | 能力/限制 | PG19 原生是否已有 | 我们当前专用路线及边界 | Neo4j 对照 | 差距归属 |
|---|---|---|---|---|---|
| G01 | 起点和展开顺序枚举 | **有关系连接顺序搜索**，重写后进入 DP/GEQO，不是图专用起点搜索。[原生搜索][P-native-search] | 从 `vertex_rtis[0]` 取起点，按固定阶段展开，没有通用起点/展开顺序枚举。[候选][P-candidate] | IDP 组合展开和连接候选，有表规模及时间预算，不是无限穷举。[IDP][N-idp] | 图专用规划待补；不是 PG 没有连接优化 |
| G02 | 无向、IN、混合方向 | **有**，无向生成正反连接条件。[原生重写][P-native-direction] | 标准入口只接受 OUT；无向不生成图 IR，IN/混合方向拒绝，底层 IN 接口不代表已接通。详见 §3。 | 支持有向、无向及混合模式。[方向][N-fixed] | 专用路线接入缺口 |
| G03 | 中间点、终点和边过滤 | **有**，转成普通查询条件。[原生过滤][P-native-quals] | 中间/终点条件、一般边条件被拒绝；首边仅放行能证明由起点键等值条件蕴含的特殊条件。[准入][P-contract] | 支持过滤，适合的点/边谓词可在变长/量化遍历中提前剪枝。[谓词][N-variable]、[展开][N-expand] | 专用路线接入及剪枝缺口 |
| G04 | 一般属性输出 | **有**，COLUMNS 映射为输出表达式。[原生输出][P-native-output] | 顶点仅身份列；边仅身份、源、目的及物理第 4 列，不是任意属性输出。[输出检查][P-output] | 可访问节点和关系属性，没有我们的物理第 4 列限制。[属性][N-fixed] | 专用路线接入缺口 |
| G05 | 索引/访问路径成本选择 | **有**普通索引候选及成本比较。[索引候选][P-native-index]、[成本][P-native-cost] | 起点复用 PG 最便宜路径；强制图路线的边索引按合格索引最小 OID 选择，不比较覆盖收益、大小和成本。[选索引][P-index] | 节点/关系索引及扫描候选按成本、hint 等选择。[候选][N-config]、[选择][N-cost-pick] | 专用图候选选优待补 |
| G06 | 两端绑定、图子计划连接 | 通用 SQL **有 JOIN/外连接**；不等于已有图 `ExpandInto`/Optional 算子体系。 | 图内部仍是单个 anchor 的 INNER 展开，没有两端绑定专用候选；M1 已能把完整图子计划接到外围 PG JOIN/外连接，这两者不混淆。[GraphPath][P-path]、[组合设计](../设计/图Btree与关系算子组合执行设计.md) | 有 `Expand(Into)`、`NodeHashJoin`、Optional 策略；Into 可按两端度数选择访问端。[算子][N-operators]、[连接][N-join]、[Optional][N-config] | 图专用算法/候选待补 |
| G07 | 异构、闭环、多路径模式 | **部分有**：异构、多标签及重复变量闭环已有用例；一个 GRAPH_TABLE 内逗号分隔多路径模式被明确拒绝。[异构][P-native-tests]、[闭环][P-native-cycle]、[多路径限制][P-native-multipath] | 线性阶段、一个顶点表及一个边映射；分支/闭环约束和异构原表不在通用准入范围。[准入][P-contract]、[映射][P-route-config] | 支持多路径组成的非线性模式与复用变量闭环。[非线性][N-nonlinear] | 混合：专用接入缺口＋原生多路径限制 |
| G08 | 变长、最短路径、路径唯一性 | **没有本表所指完整原生图能力**：量词在解析阶段直接拒绝；当前图语法没有最短路径及 TRAIL/SIMPLE 等模式入口。递归 SQL 不等于已有对应图算法。[量词限制][P-native-quantifier]、[图语法][P-native-grammar] | 固定 N 跳 WALK；IR 中存在 TRAIL/ACYCLIC/SIMPLE 名称不代表执行已接通。[IR][P-lowering]、[准入][P-contract] | 有变长/量化、最短路径规划；默认不重复关系，Cypher 25 可显式允许重复。[变长][N-variable]、[最短][N-shortest]、[重复][N-repeatable] | 原生语法及专用算法均需扩展 |
| G09 | 逐层终点去重、遍历剪枝 | 普通 DISTINCT 已有，**不等于自动把多跳 JOIN 改成逐层终点集合**。 | 已有受限 exact-k WALK 集合优化：主要识别顶层单列 DISTINCT 终点；聚合、窗口、子查询、LIMIT 等不启用。不是跨深度共享 visited，也不能替代完整路径或 TRAIL/SIMPLE。[准入][P-dedup]、[契约][P-fixed-header] | 根据路径变量用途、长度和上层语义改写 Pruning/BFSPruning，不是所有查询都去重。[规则][N-pruning] | 已有优化，覆盖范围待扩展 |
| G10 | 有序候选参与搜索 | **有**通用 pathkeys/排序规划。[原生顺序][P-native-order] | 图 Path 为 `pathkeys = NIL`，没有有序图候选供上层比较。[GraphPath][P-path] | 可保留满足顺序要求的候选并考虑排序位置，不代表所有算子保序。[IDP][N-idp-solver]、[叶子候选][N-leaf] | 图路径物理属性未接通 |
| G11 | 统计采集、消费和反馈 | **有**原表/表达式等统计及 ANALYZE；不是我们新增的图路径统计系统。 | 图统计显式采集，支持 PG 列统计回退；`path_destination()` 只装载部分指标，反馈 API 当前调用在测试模块，未形成真实执行器自动反馈闭环。[维护][P-stats-store]、[加载][P-stats-load]、[反馈][P-feedback-test] | 自动图计数、索引采样及统计变化触发的重规划；不代表逐查询学习或相关性估计总准确。[统计][N-stats] | 图特有统计维护/消费待闭环 |
| G12 | 并行执行 | **有**通用并行框架，也为 CustomScan 提供接口；不会自动并行化自定义执行器。[接口][P-native-parallel] | 图 Path 的 `parallel_safe/aware=false`、workers=0；批访问不等于并行。[GraphPath][P-path] | Enterprise/Aura 有流水线/融合及受限并行运行时，不可当 Community 默认能力。[运行时][N-runtime] | 专用执行器并行未接通 |
| G13 | 无 hint 自然选优 | **有**关系路径 CBO；原生不会发现我们尚未接入的全部图候选。 | 已有 `GgcEstimateGraphPath()`、`GgcCreateGraphPath()`、`add_path()`，显式测试配置可参加比较；强制 hint 先定 provider/索引且不保留 relational 竞争，不能证明默认自动择优。[候选][P-candidate]、[入口][P-route-planner] | 图候选参与成本选择；预算搜索不保证全局最优。[成本选择][N-cost-pick] | 有模型，但默认自动选路未闭环 |
| G14 | 成本反映覆盖/批访问收益 | **有**普通 Index Only Scan 成本，参考 `allvisfrac` 减少估算回表页。[原生成本][P-native-cost] | 缺物理图统计时按物理条目计回表、每次回表保守计一个页面组，未直接反映免回表及批访问局部性；误差大小须实测，不能凭代码给倍率。[图成本][P-cost] | 有成本比较，但本文未证明其模型涵盖我们每项优化，不能预设其估计更准。 | 图成本与已实现优化需同步 |
| I01 | 键类型与形态 | **不限定 bigint**；integer 和复合键已有图用例，其他类型仍按各自键约束核验。[原生键][P-native-keys] | 单列 bigint，相关身份/端点键非空；integer、UUID、text、jsonb、复合业务键不能直接进入当前通道。[准入][P-contract] | 可用字符串属性、复合唯一约束表达业务标识；不等于 PG 物理端点列。[约束][N-constraints] | 专用映射限制 |
| I02 | 物理列位置 | **按 KEY/SOURCE/DESTINATION 声明映射**，不要求固定列号。[DDL][P-native-ddl] | 顶点身份须第 1 列，边身份/src/dst 须第 1/2/3 列；只声明正确图映射还不够。[准入][P-contract] | 不直接对应 PG 物理列位置。 | 专用映射限制 |
| I03 | 至少四列边表 | **没有该要求**；键及属性分别声明。[DDL][P-native-ddl] | 要求边表 `natts >= 4`；只有 `id, src, dst` 三列、仅返回终点也会拒绝。[准入][P-contract] | 不存在对应的 PG 列数要求。 | 专用前端限制，不是图模型要求 |
| I04 | 顶点必须特定主键索引 | **可显式声明图 KEY**，不是强制物理第 1 列单键主键。[DDL][P-native-ddl] | 查有效、立即生效、第 1 列单键主键；只有等价 UNIQUE 索引仍不满足前端选择方式。[选择][P-vertex-index] | 不直接对应 PG 主键索引机制。 | 专用索引接入限制 |
| I05 | RLS、列权限 | **已有原生权限/RLS 路线**，图回归覆盖 RLS。[回归][P-native-rls] | 拒绝 RLS/security quals；边 provider 要求表级 SELECT，仅所需列授权可能仍不能进入。[准入][P-contract]、[provider][P-provider] | 安全机制不同，不机械对标 PG RLS。 | 专用安全适配待补，不能删除检查绕过 |
| I06 | 分区、继承、其他存储 | **已有分区/继承图用例**，DDL 允许视图、外表等元素；其他 TableAM 不作全面验收声明。[回归][P-native-partition]、[元素类型][P-native-elements] | 前端要求普通关系且无子表，B-tree provider 要求边表为 heap；分区/继承父表、外表不准入，叶子表须单独判断。[准入][P-contract]、[provider][P-provider] | 不直接对应 PG 分区/TableAM 接口。 | 专用存储接入限制 |
| I07 | 部分、表达式索引 | **通用 PG 优化器已有**，是否使用取决于谓词证明等条件。[索引候选][P-native-index] | 强制图入口拒绝这两类索引，要求指定首键/操作符族。[选择][P-index] | 索引机制不同，不按同名功能强行比较。 | 专用索引接入限制 |
| I08 | LATERAL/外层关联输入 | **有**原生 GRAPH_TABLE 关联输入用例。[回归][P-native-lateral] | 图程序/强制搜索拒绝关联外层变量和 lateral 输入，要求完整图区域；普通参数常量、底层 Rescan 不代表已接通。[重写][P-lowering]、[搜索][P-search] | 有 Apply 等规划；不等于 PG 的每种 LATERAL 形态一一对应。[候选][N-config] | 专用参数化路径未接通 |
| I09 | hint 与复杂 SQL 覆盖 | 普通 SQL **已有 UNION/递归及多表表达式**；不要与一个 MATCH 内多路径模式混淆。 | relational 已解除图 IR 要求，原生 UNION/递归可用；btree/adj 仍要求一个合格图程序。M1 的显式 btree 已验证外围 JOIN、过滤、聚合、业务表 RLS/分区 Append。多图区域、相关图输入和专用递归组合未验收，不宣称通用接入。[入口][P-route-planner]、[M0/M1](../设计/图Btree与关系算子组合执行设计.md) | 不直接对应我们自定义的 GraphRoute hint。 | 部分关闭，剩余专用组合/选优待补 |
| I10 | 同表多个图元素映射 | **可用不同别名多次声明同一表**。[DDL 别名][P-native-alias] | 同一物理边表对应多个图边元素会拒绝；不是查询中正常的 `x`、`y` 两个边变量。[配置][P-route-config] | 不直接对应 PG 图目录与原表映射。 | 专用映射配置限制 |
| I11 | 其他优化器 hook 共存 | **提供 hook**，不意味着插件之间自动兼容。[接口][P-native-search] | 已有不同 `join_search_hook` 时强制图路线报错；不能声称完成 ORCA 联合规划。本轮未改 ORCA/Daft。[入口][P-route-planner] | 不直接对应 PG 扩展接口。 | 插件协同适配待验收 |
| O01 | 公共批访问 | 原生有不同批访问/索引扫描机制，不能等同本图请求接口。 | 已有端点请求、候选、回表分组；不代表每条 SQL 都启用 B-tree 数组/范围探测等全部快路径。[运行时][P-runtime-enable]、[provider][P-provider] | Enterprise/Aura 有批流水线/融合；批的对象和粒度不必相同。[运行时][N-runtime] | 已有优化，逐计划确认实际启用 |
| O02 | 外层 DISTINCT 消除 | 普通 DISTINCT/唯一性规划已有；图输出唯一性仍需专门证明。 | 证明单起点全局唯一时可消除额外去重；多起点内部各自唯一不代表合并后唯一。[证明][P-upper-distinct] | 有基于语义的去重/剪枝规则，不据此认定同一条消除规则完全一致。[规则][N-pruning] | 已有优化，不能越过唯一性前提 |
| O03 | 覆盖索引免边回表 | **已有 Index Only Scan**，受索引可返回列及可见性条件限制。[原生成本][P-native-cost] | 已支持适合的无 payload 端点集合；VM 不满足等情况仍回表。`INCLUDE(amount)` 不等于当前读取器直接交付 amount。[准入][P-cover] | 不直接对应 PG heap/VM 机制，不由此推断谁全面免回表。 | 已有优化，投影/可见性范围受限 |
| O04 | factorized 路径表示 | 普通关系执行不等于本特性的路径 DAG 表示。 | 底层有 API/测试；标准 PGQ 普通 bag/path 分支不能算全面启用，前端条件及顶点检查仍有限制。[前端][P-runtime-enable]、[底层][P-factor-enable] | 本轮未核验完全等价的表示；流水线融合不能当作同一能力。 | 底层已有，端到端覆盖未完成 |
| O05 | 预算与溢写 | **部分原生算子支持溢写**，例如排序；不是所有内存分配都能溢写。[排序][P-native-spill] | 有候选/路径绑定/arena 检查，但 arena 超限报错，没有自动溢写后继续执行。[分配器][P-arena] | 本轮未逐算子核验；不能反推 Neo4j 所有算子都能溢写。 | 已有预算防护，缺对应溢写能力 |
| O06 | 公共执行器＋AGE 内存邻接 | **不是原生 PG19 能力**，也不是整个 AGE 语法/优化器接入。 | 默认关闭；仅准入固定 OUT WALK 端点集合。要求实际 superuser、无特殊安全上下文、RR READ ONLY、无既有写入/子事务、永久且无 RLS/继承的 heap；有 1M 扫描行与 128 MiB 周期性检查，不是零超调硬峰值证明。[适配][A-visible]、[启用][P-runtime-enable] | 不直接对应本事务私有适配器，不能用其热态倍率代表 Neo4j 对照。 | 适配器实验边界，不是原生 AGE 限制 |

读表时还需保留三条边界：

- **G07 的限制是查询模式，不是数据必须为链。** 当前固定 WALK 可以遍历多分支、有环、有平行边的数据；现有多分支树基准不与此矛盾。
- **失去优化不一定是查询不能执行。** 例如增加 LIMIT 会禁用 G09 的终点集合改写，不能因此说所有带 LIMIT 的图查询都不支持。O06 的热态收益也不能代替首次装载、事务切换和写入负载评估。
- **不能单纯放宽拒绝条件来“补齐”。** 权限、快照和语义检查必须保留；应实现对应的正确执行路径并验收。

### 4.1 这些差距中，哪些会先影响真实业务？

以“从账户 A 出发，只沿大额转账，经过高风险账户，查最终收款人”为例：

```text
起点：a.id = 1
边条件：x.amount > 10000
中间点条件：b.risk = 'high'
输出：c.id、c.name
```

PG19 原生已有这些过滤和属性输出能力，但当前专用路线会受 G03/G04 限制，尚未进入“从哪个点开始更便宜”的问题。因此不能把差距都归到起点枚举，也不能把无属性过滤基准的倍率直接套到此业务。[原生过滤][P-native-quals]、[原生输出][P-native-output]

Neo4j 能表达并规划这些条件，但实际起点、索引取决于统计和数据；本文没有运行该查询，不虚构其实际计划。[模式与属性][N-fixed]、[候选选择][N-cost-pick]

## 5. 不能误判为差距，或目前无法证明的事项

1. **不能说 PG 不支持属性、表达式统计、索引、Join 或排序。** 很多能力 PG 已有，问题在专用图路线的接入与语义归属；不应全部重造。
2. **不能说图 B-tree 没有批访问和去重。** 已有能力及其条件见 §4 的 G09、O01/O02；问题是覆盖范围和自然选优，不是完全没有。
3. **不能说我们没有 MVCC、计划缓存或 DDL 失效处理。** 已有相应检查和缓存计划重建回归；本轮没有发现可据以宣称这些能力完全缺失的证据。[现有路由回归][P-route-test]
4. **不能说 Neo4j 不使用连接、一定不重复遍历、总从最优起点开始。** 源码存在连接候选，剪枝有前提，预算搜索也不保证全局最优。[连接规划][N-join]、[剪枝][N-pruning]、[IDP][N-idp]
5. **不能把 Neo4j Enterprise 的融合/并行能力当成开源 Community 默认能力。** 不能把线程数或算子数量直接换算成加速比。[运行时][N-runtime]
6. **Cypher 最短路径与加权最短路径不是同一能力。** 官方将加权最短路径等需求另指向 GDS；本文没有把 GDS 全部算法算作普通多跳规划器的能力。[最短路径边界][N-shortest]
7. **未做同数据、同语义、同测量口径的双方 PK。** 不能给出“目前相差 2 倍/10 倍”之类结论。已有 PG 三路测试是各自相对 PG relational 的结果。
8. **本报告不是所有缺口已经穷尽的证明。** 未审计 Neo4j 全部存储、并发、集群、恢复机制；未逐项新跑本地所有 SQL、权限、数据分布和故障组合。未核验的能力应标记待验证，而不是默认支持。

## 6. 建议补齐顺序与验收方式

以下是基于上述证据的优先级建议，不代表已授权实施，也不是新增任务的完成状态。

| 优先级 | 工作 | 验收标准 |
|---|---|---|
| 第一组：能力边界与基础业务 | 建立方向/过滤/属性的能力矩阵；修正 relational hint 对专用 IR 的依赖；接通 IN、无向与必要的语义处理 | 用标准 PGQ 入口测试，不只调用底层接口；自环、双向边、平行边、空邻接结果均正确 |
| 第二组：常用 PG 图表适配 | 中间点/边谓词、一般属性交付、映射解耦；取消不必要的列位置/四列限制 | 不能丢过滤、改变 bag/path 数量、绕过 MVCC/RLS；不支持的形态给出可理解的原因 |
| 第三组：真实选优 | 合格索引成本比较；有限的起点/方向/ExpandInto 候选；与覆盖/批访问一致的成本；自然选路验证 | 普通与覆盖索引共存、数据选择性相反、两端已知等场景，不靠固定 hint 证明选优；同时记录规划时间与执行时间 |
| 第四组：扩大语义与优化覆盖 | 有证明的 DISTINCT/LIMIT/聚合等改写，异构/非线性图模式，变长/最短路径；按实际需求推进并行与大结果处理 | 各项独立定义语义和预算，不用“固定 N 很大”替代变长算法，不用内存小样本替代大图可用性 |

统计方面优先复用现有 PG 列/表达式统计，并补齐图特有信息的采集和消费；不要为了对齐 Neo4j 再建一套与 PG/AGE 原入口冲突的统计系统。

### 6.1 最小补充验证清单

下表是后续验收清单。除引用已有回归脚本的预期外，**本轮未执行这些新组合**；不将“源码预计拒绝”记录成“实测已通过”。

| 编号 | 用例 | 当前判断 / 需要核验的点 |
|---|---|---|
| V01 | 同质固定 OUT 直链、只输出 ID | 既有能力；作为正向控制组 |
| V02 | 同数据改为 IN、无向、OUT/IN 混合 | 当前专用入口拒绝；分别验证无 hint 的原生路线与三种 hint 的入口行为 |
| V03 | 自环、两方向各一条边、不同 ID 的平行边 | 扩展无向能力时必须独立校验，不能简单拼接 OUT/IN 后统一 DISTINCT 改变 bag 语义 |
| V04 | 边条件、中间/终点条件、返回 name/age/第 5 列边属性 | 当前限制见 §4；确认是图计划拒绝、普通路线可执行，还是仅优化未启用 |
| V05 | bigint 与 integer/text/UUID/复合键；调整列顺序；三列边表 | 当前限制见 §4 的 I01～I04；不能拿一种 schema 的通过推断其他映射 |
| V06 | 普通索引与覆盖索引共存、改变 OID 大小关系 | 当前按最小 OID 选择；后续需证明成本选择与实际收益一致 |
| V07 | 部分/表达式索引、仅列级授权、RLS、分区父表 | 检查安全拒绝或正确回退；不能删除安全门后只测速度 |
| V08 | 多个 GRAPH_TABLE、LATERAL、Append/递归、同表多个图元素映射 | 验证入口边界；普通 Prepared Statement 参数和外层相关绑定分开测试 |
| V09 | 终点 DISTINCT、DISTINCT+LIMIT、COUNT(DISTINCT)、多起点 | 对照结果语义，并检查实际是否启用逐层去重及是否保留必要的外层 DISTINCT |
| V10 | 完整路径/bag 输出、返回 payload、预算耗尽 | 不得偷换成终点集合；检查 factorized/覆盖路径实际开关及错误行为 |
| V11 | 无 hint 的自然 CBO、选择性高/低互换、统计缺失/过期 | 记录候选、估计与实际行数、选中索引、规划和执行时间；不能只确认有 Cost 数字 |
| V12 | AGE 开关、首次装载/事务内热态、普通角色与不同事务隔离 | 分离适配器限制和普通图路线能力，不混用冷热态性能 |

### 6.2 后续与 Neo4j 做性能对照的口径

- PG 内部三路对照：同数据、同 SQL，仅改变 route/必要测试开关。
- 跨 PG/Neo4j：两种查询语言不同，应构造业务语义等价的 SQL/PGQ 与 Cypher，而不是声称 SQL 文本相同。
- 必须固定方向、跳数/范围、路径唯一性、bag/终点集合、属性过滤、输出列、LIMIT 和结果校验方式。
- 本地当前 WALK 允许重复元素；Neo4j 默认 MATCH 不允许重复使用关系。需要时使用支持的 Cypher 25 `REPEATABLE ELEMENTS` 对齐，或明确证明测试数据上两种语义等价。[重复元素语义][N-repeatable]
- 分开报告冷编译/规划、计划缓存命中、执行、首批返回与完整消费；Neo4j 涉及代码生成时不得把该开销混进另一端不同的计时阶段。[运行时][N-runtime]
- 比较数据规模之外，还要固定可达集合、平行边、汇聚/重复路径、属性选择性、缓存热度和输出量。只有总边数相同不足以公平比较。
- 目前四场景中出现的大倍率，仅证明既定语义和范围内的收益；不能代表上述缺失能力已经实现，也不能外推最终自然 CBO、DML 和生产稳定性。

## 7. 最终判断

> 历史快照；当前判断以 §0 为准。

**我们已经拥有值得保留的公共多跳执行核心、B-tree/adj 访问能力和受限语义下的有效优化；最大的差距是从“特定查询能跑得快”到“常见图查询都能正确接入，并自动选到合适算法和访问路径”。**

当前不能只补“起点枚举”，也不能只继续追逐现有四类基准的倍率。无向/IN、属性谓词、映射适配、索引选优、优化规则适用范围及自然选路，应作为独立、可验收的能力项。

<!-- 引用紧邻对应结论；定义集中在此以避免表格重复长路径。 -->
[P-native-direction]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/rewrite/rewriteGraphTable.c:473
[P-native-search]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/optimizer/path/allpaths.c:3903
[P-native-quals]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/rewrite/rewriteGraphTable.c:536
[P-native-output]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/rewrite/rewriteGraphTable.c:558
[P-native-tests]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/regress/sql/graph_table.sql:139
[P-native-cycle]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/regress/sql/graph_table.sql:368
[P-native-keys]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/regress/sql/graph_table.sql:183
[P-native-lateral]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/regress/sql/graph_table.sql:155
[P-native-partition]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/regress/sql/graph_table.sql:481
[P-native-rls]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/regress/sql/graph_table_rls.sql:93
[P-native-quantifier]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/parser/parse_graphtable.c:243
[P-native-multipath]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/parser/parse_graphtable.c:340
[P-native-grammar]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/parser/gram.y:18172
[P-native-ddl]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/doc/src/sgml/ref/create_property_graph.sgml:139
[P-native-alias]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/doc/src/sgml/ref/create_property_graph.sgml:125
[P-native-elements]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/doc/src/sgml/ref/create_property_graph.sgml:58
[P-native-index]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/optimizer/path/indxpath.c:809
[P-native-cost]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/optimizer/path/costsize.c:545
[P-native-order]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/optimizer/plan/planner.c:1787
[P-native-parallel]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/include/nodes/extensible.h:140
[P-native-spill]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/utils/sort/tuplesort.c:34
[P-lowering]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/rewrite/rewriteGraphTable.c:659
[P-contract]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/modules/test_graph_core/test_graph_fixed.c:716
[P-output]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/modules/test_graph_core/test_graph_fixed.c:588
[P-vertex-index]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/modules/test_graph_core/test_graph_fixed.c:568
[P-dedup]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/modules/test_graph_core/test_graph_fixed.c:895
[P-upper-distinct]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/modules/test_graph_core/test_graph_fixed.c:983
[P-candidate]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/modules/test_graph_core/test_graph_fixed.c:1049
[P-search]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/modules/test_graph_core/test_graph_fixed.c:1166
[P-runtime-enable]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/modules/test_graph_core/test_graph_fixed.c:2133
[P-index]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/modules/test_graph_core/graph_route_hint.inc:219
[P-route-config]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/modules/test_graph_core/graph_route_hint.inc:265
[P-route-planner]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/modules/test_graph_core/graph_route_hint.inc:391
[P-provider]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/executor/graph_btree_provider.c:96
[P-path]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/optimizer/path/graphpath.c:65
[P-cost]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/optimizer/path/costgraph.c:160
[P-stats-store]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/statistics/graph_stats_store.c:1
[P-stats-load]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/optimizer/path/graphstats.c:164
[P-feedback-test]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/modules/test_graph_core/test_graph_cost.c:43
[P-fixed-header]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/include/executor/graph_fixed.h:132
[P-cover]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/executor/graph_fixed.c:882
[P-factor-enable]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/executor/graph_fixed.c:3446
[P-arena]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/backend/executor/graph_fixed.c:2113
[P-route-test]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/postgres19/src/test/modules/test_graph_core/sql/graph_route.sql:150
[A-visible]: /home/qiumc/data/code/cpp/.worktrees/pg19-graph-core/apache-age-pg19-port/src/backend/utils/adt/age_visible_adj.inc:1
[N-fixed]: https://neo4j.com/docs/cypher-manual/current/patterns/fixed-length-paths/
[N-idp]: https://github.com/neo4j/neo4j/blob/2026.09/community/cypher/cypher-planner/src/main/scala/org/neo4j/cypher/internal/compiler/planner/logical/idp/SingleComponentPlanner.scala
[N-expand]: https://github.com/neo4j/neo4j/blob/2026.09/community/cypher/cypher-planner/src/main/scala/org/neo4j/cypher/internal/compiler/planner/logical/idp/expandSolverStep.scala
[N-join]: https://github.com/neo4j/neo4j/blob/2026.09/community/cypher/cypher-planner/src/main/scala/org/neo4j/cypher/internal/compiler/planner/logical/idp/joinSolverStep.scala
[N-config]: https://github.com/neo4j/neo4j/blob/2026.09/community/cypher/cypher-planner/src/main/scala/org/neo4j/cypher/internal/compiler/planner/logical/QueryPlannerConfiguration.scala
[N-cost-pick]: https://github.com/neo4j/neo4j/blob/2026.09/community/cypher/cypher-planner/src/main/scala/org/neo4j/cypher/internal/compiler/planner/logical/steps/pickBestPlanUsingHintsAndCost.scala
[N-leaf]: https://github.com/neo4j/neo4j/blob/2026.09/community/cypher/cypher-planner/src/main/scala/org/neo4j/cypher/internal/compiler/planner/logical/steps/leafplanner/leafPlanOptions.scala
[N-idp-solver]: https://github.com/neo4j/neo4j/blob/2026.09/community/cypher/cypher-planner/src/main/scala/org/neo4j/cypher/internal/compiler/planner/logical/idp/IDPQueryGraphSolver.scala
[N-pruning]: https://github.com/neo4j/neo4j/blob/2026.09/community/cypher/cypher-planner/src/main/scala/org/neo4j/cypher/internal/compiler/planner/logical/plans/rewriter/pruningVarExpander.scala
[N-variable]: https://neo4j.com/docs/cypher-manual/current/patterns/variable-length-paths/
[N-shortest]: https://neo4j.com/docs/cypher-manual/current/patterns/shortest-paths/
[N-repeatable]: https://neo4j.com/docs/cypher-manual/current/patterns/repeatable-node-and-relationship-paths/
[N-nonlinear]: https://neo4j.com/docs/cypher-manual/current/patterns/non-linear-paths/
[N-operators]: https://neo4j.com/docs/cypher-manual/current/planning-and-tuning/operators/operators-detail/
[N-stats]: https://neo4j.com/docs/operations-manual/current/performance/statistics-execution-plans/
[N-runtime]: https://neo4j.com/docs/cypher-manual/current/planning-and-tuning/runtimes/concepts/
[N-constraints]: https://neo4j.com/docs/cypher-manual/current/schema/constraints/create-constraints/
