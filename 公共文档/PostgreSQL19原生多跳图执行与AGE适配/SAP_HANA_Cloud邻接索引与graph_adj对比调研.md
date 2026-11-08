# SAP HANA Cloud 邻接索引、内存机制与 graph_adj 对比调研

> 调研日期：2026-09-08  
> 读者：PostgreSQL 原生多跳图执行方案的设计与评审人员。  
> 范围：HANA Cloud 属性图、邻接索引及其内存行为；不包括 RDF、向量 HNSW 或 SQL Hierarchy 的索引。  
> 版本口径：产品事实以本次实际取得的 Cloud QRC 1/2026、QRC 2/2026 官方资料为主；历史论文单独标年，不冒充当前实现。  
> 我方基准：[主设计文档](<../02. PostgreSQL19原生多跳图执行与AGE适配详细设计.md>) 2026-09-08 的稳定端点引用方案，尤其 §10.3、§10.5.1。它仍是待原型验证的设计，不是已测成品。

## 1. 先回答：是不是主要靠“全放到内存”加速？

**内存是重要条件，但不能把 HANA 的优势简单归结为“把普通边表全放进内存”。也没有足够公开证据量化：其性能收益中，内存驻留究竟占多少。**

需要分开两个问题：

- **结构是否改变了？** 历史一手材料明确存在图专用邻接结构、内部编码寻址，以及按程序需求构造索引的优化；因此不只是改变数据所在介质。详见 §4。
- **当前产品对象是否都只是内存副本？** 不是。表图有可复用的内存索引缓存；JSON collection 上显式创建的 `ADJACENCY INDEX` 则被官方明确说明为持久化索引。二者不能混称。[SAP：Global Cache Configuration，Cloud QRC 1/2026][sap-cache]；[SAP：CREATE ADJACENCY INDEX，Cloud QRC 1/2026][sap-adj]。

与我们的方案相比，**“通过邻接结构继续多跳”这一思路相近；不能据此认定 HANA 当前也采用我们的稳定 Slot、页指针、代际和回收协议。**

## 2. 先分清：HANA 文档说的是哪一种图、哪一种索引？

当前 Cloud 数据模型区分“表上的图”和“JSON collection 上的图”。前者的数据来源不只列存表，也可以是行存表、远程表或视图；后者在 collection 内用字段区分顶点和边。[SAP：SAP HANA Graph Data Model，Cloud QRC 2/2026][sap-model]。

### 2.1 表上的图：GraphScript 使用的二级索引与 GraphCache

官方 workspace 文档说明，表图用于 GraphScript 时会构建加速访问的二级索引结构，并可在内存中保留供后续使用。其缓存选项包括：

| 选项 | 直白解释 |
|---|---|
| `WITH DYNAMIC CACHE` | 默认方式；复用索引并反映当前事务视图。全部来源为列存表时，访问时尽可能增量更新，避免整体重建。 |
| `NO CACHE` | 不保留索引供后续调用复用；不等于本次执行不使用内存。 |
| `WITH STATIC CACHE RETENTION …` | 在指定保留期内可跳过源数据变化；不能把这种可能陈旧的结果口径与我方正常 snapshot 语义直接做性能对比。 |

来源：[SAP：CREATE GRAPH WORKSPACE，Cloud QRC 2/2026][sap-workspace]。本次可读的该页是 SAP 官方机器翻译版本；缓存存在和释放行为另由英文专章核对。

GraphCache 有内存预算和 LRU 回收；官方说明超限检查默认每小时进行，并提供清理缓存的管理命令。因此它不是永久固定驻留，更不能将预算误认为任何时刻都不越过的执行内存硬上限。[SAP：Global Cache Configuration][sap-cache]。

**但是：查询之间能丢弃一个缓存索引，不等于查询进行中能把这个索引分成若干页换入换出。**

### 2.2 JSON collection：显式 CREATE ADJACENCY INDEX

以下是官方语法的示意，不是本次执行过的 SQL。假定 collection 已存在，且文档字段符合图模型约束：

```sql
CREATE HASH INDEX graph_id_hash ON graph_collection("id");

CREATE ADJACENCY INDEX graph_adj_idx ON graph_collection
    (ID "id", SOURCE "source", TARGET "target", TYPE "type");
```

官方明确：该对象用于 JSON collection，要求 ID 字段上已有相应 hash index，并写明 **“The adjacency index is persisted.”** 所以“所有 HANA 邻接索引都不落盘”是错误概括。[SAP：CREATE ADJACENCY INDEX][sap-adj]。

但“持久化”本身没有回答三个问题：执行前是否需加载完整拓扑、运行中能否部分驻留、重启如何恢复内部结构。不能自行补出答案。

### 2.3 为什么这两个说法不矛盾？

“持久化”回答的是**数据是否有持久保存的表示**；“内存执行”回答的是**计算时使用什么表示、它在哪里**。

一个索引完全可以持久化，同时在运行时大量使用内存。另一个索引也可以由持久化源数据派生，但自身只是可丢弃的缓存。这两个维度不能互相代替。

## 3. “全放到内存”至少混合了四种不同含义

| 所谓“全量” | 本次能确认什么 | 不能据此推出什么 |
|---|---|---|
| 全库所有表、所有属性 | 图来源的范围很广，不能从“内存数据库”四字推导它们永远全部常驻。 | 不能说整个 HANA 数据库必须一次性全装入内存。 |
| 某个 workspace 的完整拓扑 | 表图存在内存索引与缓存。 | 缓存可驱逐，不证明活动拓扑可分页；公开资料不足以确认所有算法的最低驻留范围。 |
| 顶点、边的所有属性 | 历史 GraphScript 会按程序需要选择复制哪些属性，详见 §4。 | 不能说每次遍历都必然复制每条边的全部属性。 |
| JSON collection 及其索引 | collection 有加载、卸载机制。 | collection 可卸载，不证明 adjacency index 可以独立卸载或按页访问。 |

最后一行依据：[SAP：Load/Unload a Collection，Cloud QRC 1/2026][sap-collection-load]。本节各层的结论不能交叉替代。

也不能用“HANA 有 NSE”填补图索引的缺口。NSE 的一般能力与某个图拓扑结构是否支持分页不是同一个命题；其限制文档还明确指出，查询中间结果不会从内存分页到磁盘。本次没有找到当前属性图 adjacency index 的专门分页支持声明。[SAP：SAP HANA NSE Functional Restrictions，Cloud QRC 2/2026][sap-nse]。

**所以，我们既不能宣称“它必须把全库、全属性都常驻”，也不能反向宣称“它已像 PG Buffer Manager 一样按图索引页调入调出”。**

## 4. 历史一手材料说明：结构和执行方式也在加速

### 4.1 邻接寻址不是每跳重新按业务键搜索

Marcus Paradies 的 2017 年博士论文 §6.4 描述的研究实现中，OUT/IN 分别组织邻接；外层数组每顶点一项，内层存邻居的稠密编码，按位置进入邻接，另用 row ID 映射连接属性列组。§4.2 中的 B+ 树则用于写优化字典的“值→编码”查找，不是每跳邻接目录。[Paradies：Graph Processing in Main-Memory Column Stores，2017，印刷页 40–41、112–115][paradies-thesis]。

这支持“业务 key 查找与内部邻接寻址可以分开”的架构判断，不支持“当前 Cloud 必定使用相同数组”。内部映射也不等于永远稳定的裸内存地址。

### 4.2 构造索引时就减少不需要的数据和解释开销

SAP 作者的 2017 年 GraphScript 论文 §2、§4 描述：

- 过程执行前按程序需求即时构造临时二级索引，包括方向、邻接顶点/边及属性需求。
- 只复制程序访问的属性；诱导子图的谓词可下推到构建阶段，缩小加载范围。
- 通过代码生成、JIT 和低层图接口减少执行热路径中的解释与间接调用开销。

来源：[Paradies 等：GraphScript: Implementing Complex Graph Algorithms in SAP HANA，DBPL 2017][graphscript-paper]。

这些是**历史实现证据**。尤其不能把“构建时只装需要的子图”改写成“当前 Cloud 会随着每一跳逐页加载邻域”，也不能把临时索引等同于 §2.2 的持久化 DDL 对象。

### 4.3 对“主要靠内存”的准确评价

从这些机制可以合理推断，性能来自多种因素叠加：邻接组织、内部引用/编码、内存访问、索引复用，以及减少不必要的数据和解释工作。

但这是机制分析，不是收益比例测量。没有控制变量的实验，不能把某一项说成占 70%、90%，也不能据此给当前 HANA 与我们的方案估算倍数差距。

## 5. 用同一份原表解释：结构优化与内存优化到底差在哪？

以下沿用主设计 §10.5.1 的数据。顶点为 10、20、30、40、50、60；边表为：

| edge_id | src_id | dst_id |
|---:|---:|---:|
| 101 | 10 | 20 |
| 102 | 10 | 30 |
| 103 | 20 | 40 |
| 104 | 30 | 40 |
| 105 | 40 | 50 |
| 106 | 50 | 60 |

四跳查询的逻辑 SQL：

```sql
SELECT e1.edge_id AS edge1,
       e2.edge_id AS edge2,
       e3.edge_id AS edge3,
       e4.edge_id AS edge4,
       e4.dst_id  AS target
FROM edge_table e1
JOIN edge_table e2 ON e2.src_id = e1.dst_id
JOIN edge_table e3 ON e3.src_id = e2.dst_id
JOIN edge_table e4 ON e4.src_id = e3.dst_id
WHERE e1.src_id = 10;
```

结果是两条路径：`101→103→105→106`、`102→104→105→106`，终点均为 60。

这里的 SQL 只定义查询含义，**不保证优化器选择 Nested Loop，也不意味着把普通 JOIN SQL 交给 HANA 就必然触发图引擎**。

### 5.1 仅改变介质：原来的搜索动作仍可能存在

假设物理计划确实是参数化 Nested Loop 加端点 B-tree。把其页面都预热到内存，主要消除了相应磁盘读取等待；每次新参数所需的定位、键比较、执行器调用等工作仍可能存在。

“全在内存”不等于“所有查找都成了数组访问”。这里是条件化的机制说明，不是 HANA 的实测执行计划。

### 5.2 改变结构：直接提供当前顶点的邻接入口

用逻辑邻接表表示同一图，可以写成：

```text
OUT(10) = [(边101, 点20), (边102, 点30)]
OUT(20) = [(边103, 点40)]
OUT(30) = [(边104, 点40)]
OUT(40) = [(边105, 点50)]
OUT(50) = [(边106, 点60)]
```

这些是“谁连接谁”的逻辑关系，**不是某个产品的页格式**。

我们当前方案为“下一点”额外保留 `VertexRef`，于是该例在同一索引、同一 build、兼容端点域且引用有效时，过程如下：

| 跳数 | 当前绑定的点 | 我们的邻接入口 | 下一步 |
|---:|---|---|---|
| 1 | 10 | 起点只有 key：目录解析到 V10，再进 Slot 的 OUT Descriptor | 得到 V20、V30 |
| 2 | 20、30 | V20、V30 直达各自 Slot，不按 key 下降目录树 | 两个 parent binding 都携带 V40 |
| 3 | 40，来自两个 parent | 对兼容请求共享 V40 的一次邻接 probe，再关联回两个 parent | 两个 binding 携带 V50 |
| 4 | 50，来自两个 parent | 同理共享 V50 的邻接 probe | 保留两条结果，终点为 60 |

每跳候选边仍需通过主设计规定的 Table AM/MVCC/RLS 及相关谓词检查，才能传播引用。共享物理 probe 不能把两条合法路径去重成一条。上述细节依据[主设计 §10.3、§10.5.1](<../02. PostgreSQL19原生多跳图执行与AGE适配详细设计.md>)，不是对 HANA 当前实现的宣称。

在这个例子里，结构优化减少的是中间跳按逻辑 key 重新定位的工作；**四跳并没有变成一跳，邻接扫描、事务检查和结果构造也没有消失。**

历史 HANA 的内部编码邻接与此有抽象相似性，但本次证据不足以给 HANA 填上同样的目录次数、probe 共享次数或物理页面跳转图。

## 6. 与我们的最终方案 B：相似到哪一层？

我方均指当前主设计的目标，不表示功能已经实现。

| 比较层次 | 判断 | 差别或证据边界 |
|---|---|---|
| 图执行理念 | 相似 | 都讨论图专用邻接访问，而不只把多跳理解为反复关系连接；HANA 的历史机制有论文证据。 |
| 端点内部寻址 | 抽象相似，不能认定实现相同 | 我们明确采用带作用域和代际的稳定 Slot；当前 Cloud 对应字段与地址稳定性协议未取得公开证据。 |
| 持久化对象 | 不能统一归类 | HANA 表图内存缓存与 collection 持久化索引应分开；我们是 PG 关系表上的持久化二级 Index AM。 |
| 缓存与驻留 | 存在重要区别 | 我们按 PG 索引页面经 Buffer Manager 访问，不将整个索引常驻作为设计前提；HANA 活动图拓扑的分页能力尚未确认。 |
| 属性与可见性 | 不能拿内存地址替代事务语义 | 我们默认不覆盖属性，候选边回 Table AM 检查；不能把 HANA 历史属性复制或 row ID 映射视作同等成本路径。 |
| 生命周期与更新 | 不能认定相同 | 我们明确设计 WAL、base/sealed/active、代际发布与延迟回收；本次资料未披露与之对应的当前 Cloud 页级协议。 |
| 性能 | 无法下产品胜负结论 | 我方尚待原型；本次没有 HANA 实库，也没有同数据、同语义、同冷热条件的对照。 |

HANA 的产品事实见 §2，历史物理证据见 §4；我方来源为[主设计 §10](<../02. PostgreSQL19原生多跳图执行与AGE适配详细设计.md>)。

另一个容易忽略的点：**即使我们的全部热页已在内存，也不能自动等同于直接遍历紧凑内存数组。** Buffer 查找、pin/锁、Slot 代际校验、段管理、heap 可见性检查都仍可能消耗 CPU。这是依据我方协议作出的成本推断，不是已经测出的性能损失。

反过来，内存索引的首次构建、缓存失效、更新维护及内存容量同样应计入总成本。不能只用它的热态遍历时间与我们的完整请求时间比较。

## 7. 如果要证明“到底主要快在哪”，应如何验证？

以下是验证建议，不是已执行的测试；本次没有连接或修改任何 HANA 实例。

### 7.1 先观测表图的缓存，不把缓存指标当成全部图内存

可在具备相应监控权限的 HANA Cloud 测试实例中查询：

```sql
SELECT HOST, PORT, CACHE_ID,
       TOTAL_SIZE, USED_SIZE, ENTRY_COUNT,
       INSERT_COUNT, INVALIDATE_COUNT, HIT_COUNT, MISS_COUNT
FROM SYS.M_CACHES
WHERE CACHE_ID = 'GraphCache';

SELECT TOP 10 *
FROM SYS.M_CACHE_ENTRIES
WHERE CACHE_ID = 'GraphCache'
ORDER BY CREATE_TIME DESC;
```

这些是只读示例，未在实库执行。`TOTAL_SIZE` 是缓存预算，`USED_SIZE` 是缓存实例已用内存；命中、未命中及失效计数可帮助解释缓存复用。该视图会按访问者权限过滤数据。[SAP：M_CACHES System View，Cloud SQL Reference][sap-m-caches]。第二条查询的 GraphCache 用法来自图引擎专章。[SAP：Global Cache Configuration][sap-cache]。

**它们不直接表示完整查询峰值内存，也不是 collection adjacency index 的专用驻留率指标。**

### 7.2 分开测四类成本

| 实验问题 | 控制什么 | 避免什么错误归因 |
|---|---|---|
| 复用索引省了多少构建成本？ | 同一表图过程、同一数据，区分首次构建与有效缓存复用；同步观察缓存计数。 | 把 SQL 编译、源表首次加载也全部算成图索引构建。 |
| 邻接结构省了多少搜索工作？ | 同数据、同查询语义、源数据均已热；分别核验关系计划和图执行路径。 | 把 JOIN 路径的冷 IO 与图路径的热内存比较，或假定优化器必选 Nested Loop。 |
| 内存容量是否成为边界？ | 固定查询，逐步改变图规模与可用内存，记录构建峰值、执行峰值、失败和驱逐。 | 仅看到缓存驱逐就声称活动图可分页；仅看到内存不足就声称必须全库常驻。 |
| 更新与正确性付出多少代价？ | 相同可见性、属性过滤、路径重复语义，加入并发 INSERT/UPDATE/DELETE。 | 使用可能陈旧的 static cache 与新鲜事务结果比速度。 |

不能通过“第一次减第二次”就分离所有贡献；还需控制编译缓存、源表加载、结果规模和并发。系统级调参、清缓存、重启等动作应只在授权的隔离环境中实施。

对我方原型，优先核对 `key_directory_lookup`、`VertexRef direct`、邻接页访问、Table AM recheck、WAL 与 DML 尾延迟。这样才能判断最终方案 B 的收益是否被其他成本抵消。

## 8. 对当前设计的评价、建议与最终结论

如果“这个设计”指我们当前的方案 B，我的评价是：**核心架构是高质量的，但还不能据此认定实现已经达到优秀水平。**

它的优点具体在于：把首跳 key 解析与中间跳引用寻址分开；用稳定 Slot 隔离目录分裂与 Descriptor 位置变化；将逻辑身份、VertexRef 和 heap root TID 各自限定用途；没有因图遍历加速而取消事务检查和恢复约束。这些选择有清晰的因果关系，不只是堆叠功能。[主设计 §10.3](<../02. PostgreSQL19原生多跳图执行与AGE适配详细设计.md>)。

最需要保留的审慎判断是：双向条目、Slot 创建与回收、代际维护和 WAL 都会增加工作，是否能满足主设计要求的 INSERT 吞吐及尾延迟不劣化，尚未被证明。稳定引用省掉的目录查找，也可能不是某些高扇出、重属性过滤查询的主要成本。它是值得推进验证的优秀架构候选，不是“所有图查询都更快”的保证，也没有证据支持宣称其结构原创或与 HANA 完全相同。

### 8.1 是否可能出现较大的性能问题？

**可能，且不能只当成实现阶段的小优化。我的首要担忧是写入维护；读侧则是回表与工作集局部性。** 以下是依据设计协议作出的风险判断，不是已观察到的回归：

| 风险 | 何时可能放大 | 严谨的判断 |
|---|---|---|
| 写入维护与热点竞争 | 新 endpoint 多、并发写同一高热顶点、checkpoint 或合并同时发生 | Slot、双向条目、WAL 与后台维护都需付费；不保证一定比两个窄 B-tree 更慢，但“INSERT 不劣化”是最需要实测的硬目标。 |
| 候选边回表检查 | 扫描边多、heap 分散、属性过滤后只留下少量边 | 邻接连续不等于 heap 连续；省掉目录下降后，可见性及谓词检查可能主导时间。它也不是我方独有的新增成本，必须与同语义基线比较。 |
| 超内存工作集与多层访问 | Slot、邻接和 heap 的热点集合装不下缓存 | 直接引用省搜索，不保证目标页已在内存；可能出现较高随机页访问成本。不能机械计成“每跳固定三次 IO”，因为页可共享、内联、预取或命中。 |
| 合并与回收债务 | 持续更新叠加长事务、慢 reader，后台追不上前台 | delta/retired 增长会影响读取、空间和尾延迟；达到上限后的背压仍算业务成本。 |
| 路径数量膨胀 | 高分支且要求保留所有合法路径 | probe sharing 可省物理访问，不能省掉合法 binding 与输出；这是查询固有复杂度，不是换邻接结构就能消除的缺陷。 |

依据：[主设计 §7.2、§7.10、§10.3–10.6](<../02. PostgreSQL19原生多跳图执行与AGE适配详细设计.md>)。其中 §7.10 已将写维护、WAL、合并、回收和争用计入成本；不能以后台执行为由把它们记为零。

我的条件化预期是：**对重复拓扑遍历、缓存较热、更新压力适中的负载有信心；对频繁写入、热点争用、重属性过滤和超内存工作集，不会仅凭当前设计承诺明显优势。** 判断风险是否真的“大”，必须由原型的成本分解和尾延迟数据决定。

### 8.2 建议

这次调研**不构成把方案改为“整图常驻内存”的依据**。我建议继续验证已经选定的 B：

1. 先证明稳定引用确实消除了兼容中间跳的 key 目录下降。
2. 再优化有界缓存、批处理和页局部性，同时保留完整的可见性、代际与回收协议。
3. 分别报告冷启动、热态、超内存工作集和更新负载；不要只凭热态多跳宣称整体优于关系基线或 HANA。

仍待 SAP 当前实现说明或实库验证的关键项是：collection adjacency index 的物理寻址结构、活动拓扑的最小驻留范围及分页能力、精确恢复过程，以及各项优化的实际收益比例。

**一句话：我们与 HANA 的邻接执行方向相近；HANA 的速度不能简化为“全放内存”，两边的存储实现也不能认定相同。已核实的是对象分类、缓存行为和历史邻接机制，未核实的是当前 Cloud 的页级细节与性能归因比例。**

### 调研与交付边界

本次使用 SAP 官方 Cloud 文档、作者一手论文和主设计交叉核对。部分 SAP 动态页面直接访问只返回空壳，采用可检索的官方正文；workspace 缓存选项使用官方机器翻译版本并另行核对英文缓存专章。对未取得明确证据的内部实现保留未知，没有以二手说法补全。

相关版本已逐项标注，不宣称覆盖 2026-09-08 当天所有未取得的产品修订。后续定向检索已主要返回重复材料，无法填补活动拓扑分页和当前页格式的缺口，因此在这些边界上停止。没有实库测试，没有修改主设计或源码。

交付为 Markdown；已检查文档结构、本地引用和来源对应关系。当前环境未提供 Markdown 渲染组件，未进行页面级视觉检查。

[sap-adj]: https://help.sap.com/docs/hana-cloud-database/sap-hana-cloud-sap-hana-database-property-graph-engine-reference/create-adjacency-index-statement-data-definition-040-011-000-create_adjacency_index?locale=en-US&state=PRODUCTION&version=2026_1_QRC "SAP SE，CREATE ADJACENCY INDEX Statement (Data Definition)，Cloud QRC 1/2026"
[sap-model]: https://help.sap.com/docs/hana-cloud-database/sap-hana-cloud-sap-hana-database-property-graph-engine-reference/sap-hana-graph-data-model-030-000-000-sap_hana_graph_data_model "SAP SE，SAP HANA Graph Data Model，Cloud QRC 2/2026"
[sap-workspace]: https://help.sap.com/docs/hana-cloud-database/sap-hana-cloud-sap-hana-database-property-graph-engine-reference/create-graph-workspace-statement-data-definition-040-010-000-create_graph_workspace?locale=ko-KR&state=PRODUCTION&version=2026_2_QRC "SAP SE，CREATE GRAPH WORKSPACE Statement (Data Definition)，Cloud QRC 2/2026，官方机器翻译"
[sap-cache]: https://help.sap.com/docs/hana-cloud-database/sap-hana-cloud-sap-hana-database-property-graph-engine-reference/global-cache-configuration-040-050-000-cache_configuration?q=RFC+Vertex "SAP SE，Global Cache Configuration，Cloud QRC 1/2026"
[sap-collection-load]: https://help.sap.com/docs/hana-cloud-database/sap-hana-cloud-sap-hana-database-json-document-store-guide/load-unload-collection?locale=en-US "SAP SE，Load/Unload a Collection，Cloud QRC 1/2026"
[sap-nse]: https://help.sap.com/docs/hana-cloud-database/sap-hana-cloud-sap-hana-database-administration-guide/sap-hana-cloud-nse-functional-restrictions "SAP SE，SAP HANA NSE Functional Restrictions，Cloud QRC 2/2026"
[sap-m-caches]: https://help.sap.com/docs/hana-cloud-database/sap-hana-cloud-sap-hana-database-sql-reference-guide/m-caches-system-view "SAP SE，M_CACHES System View，Cloud SQL Reference，本次访问2026-09-08"
[paradies-thesis]: https://marcusparadies.github.io/files/thesis.pdf "Marcus Paradies，Graph Processing in Main-Memory Column Stores，博士论文，2017，§4.2、§6.4"
[graphscript-paper]: https://www.researchgate.net/publication/319370706_GraphScript_implementing_complex_graph_algorithms_in_SAP_HANA "Marcus Paradies、Cornelia Kinder、Jan Bross、Thomas Fischer、Romans Kasperovics、Hinnerk Gildhoff，DBPL 2017，DOI 10.1145/3122831.3122841，作者上传全文"
