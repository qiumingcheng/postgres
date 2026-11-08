# `graph_adj` 格式版本与代际发布机制说明

> 本文只依据《[02. PostgreSQL19原生多跳图执行与AGE适配详细设计](../02.%20PostgreSQL19原生多跳图执行与AGE适配详细设计.md)》整理，主要展开原文第 10.4.3～10.4.10 节及第 10.8 节。OUT/IN 的完整引用与方向校验设计以原文新增的 [10.4.10 节](../02.%20PostgreSQL19原生多跳图执行与AGE适配详细设计.md#graph-adj-page-links) 为准。  
> 文中 block number、generation 数字和边数据都用于解释机制，不代表最终实现中的固定值。V1 目前是可行性原型参考结构；原文还要求 GS-010 冻结页面和引用的准确编码。本说明中的 generation 7/8 指 OUT(10) 的邻接读视图，不代表 Meta、IN 目录和所有数据页的 generation 都取相同数值。

## 1. 先给结论

`format_version` 和 `generation` 解决的是两个完全不同的问题：

| 概念 | 它回答的问题 | 什么时候通常保持不变 | 什么时候发生变化 |
|---|---|---|---|
| `format_version` | 磁盘上的这些字节应该按哪一种页面格式解释？ | 普通插入、Delta 追加、Base 合并 | 页面字段、编码或布局发生不兼容变化 |
| `Descriptor.generation` | 当前读者应该跟随哪一套自洽的 Base/Delta 页面引用？ | 在同一套已发布邻接读视图内 | 新的邻接读视图被发布，例如 Base 合并后切换 Descriptor |

最直白的类比是：

```text
format_version        = “文件格式是不是同一种格式”
Descriptor.generation = “这个方向、这个 endpoint 的邻接现在使用哪一版读视图”
```

例如，某次后台合并前后都使用 V1 页面格式：

```text
合并前：format_version=1，OUT(10) Descriptor.generation=7
合并后：format_version=1，OUT(10) Descriptor.generation=8
```

格式没有变化，所以 `format_version` 不变；但是有效的 Base 页和 Descriptor 已经换了一套，所以 `generation` 变化。

## 2. 为什么会有“第几代索引结构”

### 2.1 原始边表才是权威数据

假设 Edge Heap 中有两条边：

| Heap root TID | src | dst | weight |
|---|---:|---:|---:|
| `(42,7)` | 10 | 20 | 3 |
| `(43,2)` | 10 | 30 | 5 |

它们表示：

```text
10 ──weight=3──> 20
10 ──weight=5──> 30
```

`graph_adj` 不是第二份权威图数据，只保存邻接候选：

```text
(opposite_key, edge_heap_root_tid, flags)
```

因此 OUT(10) 最终应该能够给出：

```text
(20, (42,7), flags)
(30, (43,2), flags)
```

完整属性和最终 MVCC 可见性仍然通过 root TID 回到 Edge Heap 检查。

### 2.2 Base 和 Delta 为什么同时存在

文档把一个 endpoint 的邻接数据分成 Base 与 Delta 两层，其中 Delta 又分为 active 和 sealed 两种状态：

| 部分 | 直白含义 | 主要特点 |
|---|---|---|
| Base | 已经整理好的稳定邻接集合 | 适合连续扫描；已发布的一代不原地改写 |
| active Delta | 最近新插入、尚未整理的邻接 | 前台主要向尾部追加 |
| sealed Delta | 已停止接收写入、等待合入新 Base 的 Delta | 内容固定，可以参与后台合并 |

这样设计是为了避免两种极端：

```text
每插入一条边都移动一个有序大数组
    → 热点顶点写入代价高

只追加、永远不整理
    → Delta 链越来越长，读取越来越慢
```

所以前台主要追加 Delta，后台再把稳定 Base 与 sealed Delta 合并成新 Base；实际读写收益仍须经过原文规定的测试。

问题也由此产生：后台构建新 Base 时，旧查询可能仍在读取旧 Base。系统不能直接覆盖旧页，而要同时管理“旧的一套”和“新的一套”。这就是 generation 存在的核心原因。

## 3. 先定义 generation 8 要做成什么样子

在讲 generation 7 如何变成 generation 8 之前，先把目标说清楚。

### 3.1 当前要解决的问题

仍使用前面的两条边：

```text
10 → 20，root_tid=(42,7)
10 → 30，root_tid=(43,2)
```

假设它们当前分散在：

```text
旧 Base block 2：
    (20, (42,7), flags)

active Delta block 3：
    (30, (43,2), flags)
```

现在查询 OUT(10)，必须同时读取 block 2 和 block 3。随着持续插入，Delta 会继续增长、出现更多页或 lane。如果永远不整理，读取需要访问的 Delta 会越来越多。

但是系统也不能每插入一条边就重排 Base。这样会让热点 endpoint 的前台写入承担移动大数组和重写页面的成本。

因此当前要解决的核心问题是：

```text
把已经稳定的 Base 和旧 Delta 整理成新的 Base；
构建期间让新写入进入新 active Delta，让旧查询仍能读完原视图。
封存和发布需要短内容锁，不能理解为全程没有等待。
```

### 3.2 generation 8 的目标形态

generation 8 完成后，希望 OUT(10) 的当前结构变成：

```text
Descriptor OUT(10):
    generation  = 8
    base_head   = block 8
    sealed_head = 无
    active_head = block 6

block 8 · 新 Base · generation 8
    (opposite=20, root_tid=(42,7), flags)
    (opposite=30, root_tid=(43,2), flags)

block 6 · 新 active Delta
    接收合并开始之后的新写入
```

这里的目标不是“把数字 7 改成 8”，而是完成四件具体事情：

1. 旧 Base block 2 和旧 Delta block 3 中截至合并 cutoff、尚未达到 PG 全局回收条件的物理候选，都完整进入新 Base block 8。尚未提交的候选也不能仅因 compactor 看不见而删除。
2. 合并期间和合并之后的新写入不再进入旧 Delta，而是进入新 active Delta block 6。
3. 新查询只要取得 generation 8 的 Descriptor，就能得到一组自洽的 `新 Base + 新 Delta 边界`。
4. generation 7 的旧页面暂时保留，让发布前已经开始的查询继续安全读取；不能立即覆盖或复用。

可以把前后状态压缩成一张表：

| 项目 | generation 7 | generation 8 的目标 |
|---|---|---|
| 稳定 Base | block 2，只含邻居 20 | block 8，已整理邻居 20、30 |
| 旧增量 | block 3 含邻居 30 | 已合入 block 8，不再属于新读者的当前 Delta |
| 新写入入口 | block 3 | block 6 |
| 新查询入口 | generation 7 Descriptor | generation 8 Descriptor |
| 旧查询 | 正在读 block 2/3 | 仍允许读完它已经 pin 的 generation 7 |
| 旧页处置 | 当前有效 | 先 Retired，安全后才能 Free |

### 3.3 generation 8 具体解决什么

第一，整理读路径：

```text
原来：旧 Base + 越来越多的旧 Delta
目标：新的连续 Base + 较短的新 active Delta
```

第二，避免原地覆盖：新 Base 在新页面构建，generation 7 的读者不会看到构建到一半的 block 8。

第三，给并发读者明确边界：generation 7 的读者继续使用旧视图，generation 8 发布后的新读者使用新视图，不能把两代 heads 随意混合。

第四，允许写入继续：合并旧 Base/Delta 时，新边进入 block 6，不需要等待整个 Base 构建结束。

第五，支持安全回收：只有旧 generation 没有 reader 和其他维护引用后，旧页面才从 Retired 进入 Free。

### 3.4 generation 8 不解决什么

generation 8 只解决“物理邻接候选组织和发布”的问题，它不负责：

- 判断某条边对事务是否可见；这由 PostgreSQL Snapshot、Table AM 和 Heap MVCC 负责；
- 给边分配永久逻辑 ID；V1 仍通过 `(edge_relid, root_tid)` 定位物理候选并回表；
- 改变磁盘字段布局；本例前后 `format_version` 始终为 1；
- 把整个索引升级成另一种格式；generation 7 → 8 是同一格式内部的代际发布，不是 format upgrade。

目标明确以后，下面再看 generation 7 的具体起点，以及系统怎样安全发布到 generation 8。

## 4. generation 7：合并前的页面状态

为了直观，假设 OUT 目录中 endpoint 10 的 Descriptor 是：

```text
OUT directory leaf：endpoint=10

GgaDescriptorV1:
    generation  = 7
    base_head   = block 2
    sealed_head = 无
    active_head = block 3
    lane_count  = 1
```

相关数据页：

```text
block 2 · Base · generation 7
    (opposite=20, root_tid=(42,7), flags)

block 3 · active Delta · generation 7
    (opposite=30, root_tid=(43,2), flags)
    published_items=1
```

这里每个字段的意思是：

| 字段 | 当前例子的含义 |
|---|---|
| `generation=7` | Descriptor 描述的是第 7 代物理邻接读视图 |
| `base_head=2` | 第 7 代稳定 Base 从索引 block 2 开始 |
| `sealed_head=无` | 当前没有待合并的 sealed Delta |
| `active_head=3` | 新写入追加到索引 block 3 |
| `lane_count=1` | endpoint 10 当前使用一条追加 lane |
| `published_items=1` | block 3 中只有前 1 个完整条目允许读者看到 |

一个此时打开的查询 Q1 会固定一份自洽的物理视图：

```text
Q1 的物理候选视图
    = 读视图 7 引用的 Base
    + 读视图 7 引用的相关 sealed Delta
    + 打开请求时 active Delta 的已发布前缀
```

本例中，Q1 得到邻居 20 和 30，然后再按自己的 PostgreSQL Snapshot 回表做 HOT、MVCC、RLS、权限和谓词检查。

## 5. 后台如何从 generation 7 生成 generation 8

### 5.1 不能直接覆盖旧 Base

如果后台直接修改 Q1 正在扫描的 block 2，就需要另行解决读者位置、条目移动和并发保护问题；不能一边覆盖旧数组，一边假设旧扫描完全不受影响。

原设计因此规定：已发布的 Base generation 不原地改写。后台在新页面中构建完整的新 Base，最后只切换 Descriptor。

### 5.2 第一步：封存旧 Delta，切换新 active Delta

后台先把 block 3 从 active 改为 sealed，不允许再向它追加。然后给 endpoint 10 切换一个新的 active Delta，例如 block 6：

```text
block 3 · sealed Delta
    (opposite=30, root_tid=(43,2), flags)

block 6 · new active Delta
    暂时为空，后续新边写到这里
```

切换新 active Delta 的意义是：后台整理旧数据时，前台仍可继续接受新写入，不必等整个合并完成。

### 5.3 第二步：在新页上构建新 Base

后台在内容锁外合并：

```text
旧 Base block 2
    + sealed Delta block 3
    ↓
新 Base block 8
```

构建结果：

```text
block 8 · Base · generation 8
    (opposite=20, root_tid=(42,7), flags)
    (opposite=30, root_tid=(43,2), flags)
```

在新 Base 完整构建、WAL 条件满足并通过 generation/maintenance epoch 检查之前，普通读者仍不能把 block 8 当作正式读视图。

### 5.4 第三步：发布新 Descriptor

准备完成后，系统通过受 WAL 保护的发布操作把 Descriptor 切换到新一代：

```text
发布前：
    generation  = 7
    base_head   = block 2
    sealed_head = block 3
    active_head = block 6

发布后：
    generation  = 8
    base_head   = block 8
    sealed_head = 无
    active_head = block 6
```

这一步不是把每一条边重新“提交”一次，而是宣布：

```text
从现在开始，新读者使用 block 8 代表已经整理好的邻接，
不再把 block 2 和 block 3 作为当前 Base/sealed 组合。
```

本例能确定的是 `OUT(10).Descriptor.generation` 从 7 变为 8。`Meta.build_generation` 的值、IN(20) 的视图编号以及 block 6 的 lane/页代际不能由此推出，也不要求一起变成 8。

特别是 block 6：它在新 Base 发布之前已经接收新写入，发布后仍由新 Descriptor 引用。读视图 8 必须保留它及其合法发布边界；不能为了让数字相等而重写所有旧页。原文第 10.8 节明确要求发布时保留 `current_active_lanes`，第 10.4.10 节补充了按目标引用校验代际的规则。

## 6. 发布时，旧查询和新查询分别读什么

现在假设：

```text
Q1 在发布前已经打开并 pin 住 generation 7
Q2 在发布完成后才打开
```

它们读取的页面应该是：

| 查询 | 固定的代际 | 读取的主要页面 |
|---|---:|---|
| Q1 | 7 | 旧 Base block 2，以及它打开请求时捕获的相关 Delta 边界 |
| Q2 | 8 | 新 Base block 8，以及新 Descriptor 指向的 Delta 边界 |

关键点是：发布 generation 8 不会强迫正在运行的 Q1 中途跳到新页面。否则 Q1 可能把一半旧结构和一半新结构拼起来。

### 6.1 错误混搭会发生什么

新 Base block 8 已经包含 block 3 中的邻居 30。如果错误地读取：

```text
新 Base block 8 + 旧 sealed Delta block 3
```

物理候选 30 会出现两次。扫描协议虽然还会按 `(edge_relid, root_tid)` 消除同一请求内的 Base/Delta 物理副本，但 Descriptor 本身仍必须提供自洽视图，不能把去重当成随意混代的理由。

反过来，如果错误地读取：

```text
旧 Base block 2，但没有读取旧 Delta block 3
```

邻居 30 就会丢失。Heap recheck 只能删除不可见的 false positive，不能补出索引没有返回的候选。

因此 generation 的直接价值是：

```text
防止“新 Base + 旧 Delta”
防止“旧 Base + 缺失的 Delta”
防止 reader 沿用已经失效的页面引用
```

## 7. 为什么旧页面要先 Retired，再 Free

generation 8 发布后，block 2 和 block 3 已不属于新查询的当前读视图，但 Q1 仍可能正在使用它们。因此先在 `GGA_RETIRED` 记录页中登记它们：

```text
当前页面：block 8、block 6
退休记录：记录 block 2、block 3 的引用、原类型/方向、代际和回收条件
旧数据页：block 2 仍是 Base，block 3 仍是 sealed Delta，供 Q1 继续读取
```

只有满足以下条件后，旧页才可以进入 `GGA_FREE`：

```text
没有当前 Descriptor、旧 reader 或构建/维护流程仍引用这些页或段
+ 如果是共享 packed 页，页内所有段都已经允许释放
+ 满足索引页的安全回收协议
```

退休状态由记录页保存，不能把旧 reader 正在使用的 Base/Delta 页就地改成 `GGA_RETIRED`。共享 packed 页中一个 endpoint 的段退休，也不代表整页可以释放。

进入 Free 后，该 block 才能被重新初始化并用于别的内容。重新使用时必须重新设置 page header、`GgaOpaqueV1`、direction 和相应分配代际，并清零 reserved/padding。旧 OUT 页安全释放后也可以分配给 IN。

Heap 元组是否可以回收另按 PG VACUUM/snapshot horizon 判断。索引页引用是否退出与 Heap 版本是否可回收是两种条件，不能仅凭当前 compactor 的 Snapshot 决定二者。

如果不等待旧读者就重用 block 2，Q1 手中的“block 2”可能已经变成另一种页面或另一代数据。generation、reader pin 和延迟回收共同防止这种情况。

## 8. `format_version` 为什么不能由 generation 代替

继续使用上面的例子。generation 7 和 generation 8 的 `GgaEntryV1` 都按同一种方式解释：

```text
GgaEntryV1 = opposite_key + edge_heap_root_tid + flags
```

所以两代页面都可以是：

```text
format_version=1
```

以后如果正式磁盘格式发生不兼容变化，例如字段边界、编码或页面主体解释方式改变，那么读旧页面时不能只看 generation。generation=8、generation=100 都不能告诉代码某个字节到底是 `flags` 还是另一个字段。

此时需要独立的 `format_version`：

```text
代码支持 format_version=2
磁盘页面是 format_version=1
    ↓
若没有兼容读取路径，立即拒绝打开
    ↓
不能猜测旧格式，也不能继续解释 out_root、entry 或 descriptor
```

因此：

```text
format_version 管“如何读字节”
generation     管“应该读哪套页面”
```

把二者合成一个字段在逻辑上会混淆两种完全不同的变化：普通 Base 合并不应被误认为磁盘格式升级，磁盘格式升级也不能只被当作普通内容发布。

## 9. 各层 generation 如何配合

原设计在 Meta、Descriptor、页/段和 lane 元数据中涉及代际。它们的作用范围不同，“相互校验”不能简化为“所有数字必须相等”：

| 位置 | 字段或范围 | 直白含义 |
|---|---|---|
| Meta 页 | `build_generation` | 索引整体的构建/发布元数据；不要求局部邻接每次合并都推进它 |
| 某方向、某 endpoint 的 Descriptor | `generation` | 现在公开的是哪一套 Base/sealed/active 引用关系 |
| 页面、endpoint 段、lane | 对应结构和分配代际 | 这条引用是否仍指向它原本要访问的对象，是否已被替换或复用 |

一次读取时，读者按如下方式验证：

```text
从 OUT 目录取得 OUT(10) 的 Descriptor 读视图 8
    ↓
捕获并 pin 它实际引用的 Base、sealed 段、active lanes 和各发布边界
    ↓
每个目标分别验证 kind、direction、endpoint/lane、字段范围和预期代际
    ↓
按捕获的视图扫描，不从另一次 Descriptor 读取中拼接 heads
```

例如，新 Base 的引用预期目标是 block 8 的某一页/段生命周期，而 block 6 的引用预期的是先前已经建立的 active lane 生命周期。二者都可以被视图 8 引用，无需把各自页头的数字强行改为 8。共享 packed 页也可能被多个 endpoint 的不同视图引用，不能要求其页代际等于所有 Descriptor.generation。

只有目标与**该引用记录或协议确定的预期代际**不符时，才视为过期或错误。合法分裂/发布导致的 hint 过期应重新定位；受保护的有效引用仍指向错方向、错 endpoint 或已复用页时应报结构错误，不能反复重试或当成没有邻居。

原文第 10.4.10 节已经明确这种分工；编号和配套引用字段的准确落盘编码仍由 GS-010 冻结。先前说明中“Descriptor.generation 必须等于所指页的 generation”的例子不适用于一般共享页和跨发布保留的 active lane，本节以按目标引用校验的规则替代。

## 10. generation 与 MVCC Snapshot 不是一回事

这是最容易混淆的地方。

### generation 管物理候选结构

它决定：

```text
这次扫描应该读哪些 Base 页
应该读哪些 sealed Delta
active Delta 最多读到哪个已发布边界
哪些旧页暂时不能回收
```

### PostgreSQL Snapshot 管事务可见性

它决定：

```text
候选边对应的 Heap 元组是否已提交
当前事务是否应该看到旧版本还是新版本
是否应沿 root TID 追随 HOT 链
RLS、权限和谓词检查是否通过
```

因此正确过程是：

```text
generation 固定一套完整的物理候选来源
    ↓
扫描得到 (opposite_key, root_tid, flags)
    ↓
Snapshot + Table AM 检查真正可见的 Heap 边版本
```

generation 不能代替 Snapshot；Snapshot 也不能修复索引漏掉的候选。原文明确指出：Heap recheck 只能删除 false positive，不能补回 provider 漏掉的边。

## 11. generation 与 `published_items` 也不是一回事

active Delta 仍在同一代内部不断追加新条目。如果每追加一条边都发布新 generation，代际切换会过于频繁。原设计对页内追加使用另一个边界：`published_items`。

一次 Delta append 的顺序是：

```text
1. 在页锁内完整写好新 entry
2. 把 published_items 推进到包含新 entry 的位置，期间仍持有内容锁
3. 由同一个 WAL record 覆盖完整 entry 和新 published_items 的最终页面状态
4. 设置 page LSN
5. 释放内容锁
```

读者只能看到 `published_items` 以内的完整前缀。

所以二者分工是：

| 机制 | 控制范围 |
|---|---|
| `published_items` | 同一个 active Delta 页内，哪些追加条目已经完整发布 |
| `generation` | Descriptor 指向的整套 Base、sealed Delta、active Delta 组合 |

## 12. 常见误解

### 误解一：generation 是事务号

不是。事务可见性由 PostgreSQL Snapshot 和 Heap MVCC 决定。

### 误解二：generation 是边 ID

不是。`graph_adj` V1 没有保存永久 edge ID；边候选通过 `(edge_relid, root_tid)` 回到可见 Heap 元组。

### 误解三：每插入一条边都要产生新 generation

原设计不是这样。普通前台插入主要追加 active Delta，并推进 `published_items`；构建并发布新 Base/Descriptor 时才涉及代际发布。

### 误解四：发布新 generation 后可以立即覆盖旧页面

不可以。旧 reader、长 Snapshot 或维护引用仍可能需要旧页。旧页先进入 Retired，满足全局安全条件后才能进入 Free。

### 误解五：有 Snapshot 就不需要 generation

Snapshot 能判断某个 Heap 元组是否可见，但不能告诉扫描器应组合哪一套 Base/Delta 页面。引用校验、读视图生命周期保护和延迟回收共同防止访问已被复用的旧 block；仅比较 generation 数字也不够。

### 误解六：有 generation 就不需要 `format_version`

generation 不描述字段布局。遇到不兼容页面格式时，代码必须依靠 `magic` 和 `format_version` 拒绝错误解析。

## 13. 用一句完整的话重新定义

“当前发布的是第几代索引结构”的完整意思是：

> 对某个方向和 endpoint，目录中的 Descriptor 当前公开了一组彼此匹配的 Base、sealed Delta、active Delta 及可见边界；这组引用带有 generation。新结构构建完成后，通过受 WAL 保护的发布动作切换到新 generation。旧查询继续使用其已经 pin 的旧 generation，旧页面在所有引用安全退出前只进入 Retired、不能复用。无论使用哪一代，最终边可见性仍由 Edge Heap、Table AM 和 PostgreSQL Snapshot 决定。

## 14. 当前设计仍未冻结的部分

依据源文档，现在可以确定的是：

- `format_version` 与 generation 是两个不同的逻辑维度；
- Descriptor 是 endpoint 邻接读视图的入口；
- Base、sealed Delta 和 active Delta 必须组成自洽视图；
- 已发布 Base 不原地改写；
- 新 generation 发布后，旧页延迟回收；
- 页面必须带可校验的 kind、format version、generation 和 rightlink；
- 原文第 10.4.10 节补充页面 direction、endpoint 段定位与按目标引用校验代际的契约；
- WAL/redo 必须覆盖 Base build/publish、Descriptor 更新及 retired/free 转换。

仍待 GS-010 冻结的是：

- 精确的字段字节偏移和对齐；
- 每页可容纳的条目数；
- packed 小邻接的准确编码；
- split 和 WAL record 的准确字节格式；
- generation 的最终编号、推进和回绕规则；
- `flags` 的具体 bit 定义。

因此，本文解释的是设计机制和字段职责，不能当作已经交付的可安装 Index AM 磁盘 ABI。
