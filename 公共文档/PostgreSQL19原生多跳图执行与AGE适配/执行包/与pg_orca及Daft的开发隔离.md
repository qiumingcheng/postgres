# 与 pg_orca、Daft 的开发隔离

本图特性在独立的原生 PG19 + AGE 环境开发。现有 ORCA/Daft 集成不作为源码修改、构建安装或数据库测试目标。隔离已经由脚本检查，并在 baseline/candidate 两套实例实测；同机资源干扰和未来合并兼容性仍须分别处理。

2026-09-30最新断点：D026仅在独立 `btree-tail`（55479/55499/55500）验证，宽属性回退后主树精确恢复array v2；D027仅在 `btree-array`（55478/55497/55498）追加诊断，未保留新内核改动。两套冻结安装/数据保留，所有图服务停止；openGauss PID1638720及ORCA/Daft、原PG19/AGE、candidate原状。矩阵仍tail v2、主树array v2，不将版本混用或提前迁移到既有栈。测量不停止其他项目抢资源，诊断结果不证明跨功能兼容或DML非劣化，详见[报告§15～16](golden/G1-080/图Btree四跳100行性能与10倍可行性评估.md)。

## 现有集成为何不会自动进入新图实例

冻结的原始 `postgres19` 位于 `7873db5369b967dd53984df34fd800003410d04f`，相对 HEAD 的补丁为空。原 ORCA/Daft 集成通过扩展接入：[cluster.sh](../../../integration/pg19-pgorca-daft/cluster.sh) 设置 `shared_preload_libraries='pg_orca'`；[pg_orca.cpp](../../../pg_orca/pg_orca.cpp) 的 `_PG_init()` 注册 planner、ExecutorStart 和 EXPLAIN hook；[Daft loader](../../../pg_orca/compat/executor/daft_bridge_loader.cpp) 按需加载 Bridge。

因此，共用干净的 PG19 上游提交不等于共用扩展运行状态。每个新实例使用自己的 PG 可执行文件、`$libdir`、PGDATA 和启动配置。仅把 `pg_orca.daft_enable` 设为 off 还不足以证明原生 PG planner 独立性：ORCA 的 hook 注册发生在扩展加载时。本图实验环境要求不加载 ORCA/Daft。

## 各层隔离位置

下表路径均相对 `/home/qiumc/data/code/cpp`。

| 层次 | 既有 ORCA/Daft 集成 | 本图项目 |
|---|---|---|
| 源码 | `postgres19/`、`pg_orca/`、`daft/` | 候选 `.worktrees/pg19-graph-core/{postgres19,apache-age-pg19-port}`；基线 `.build/pg19-graph-core/baseline/src/` |
| 构建 | `.build/pg19-pgorca-daft/` | `.build/pg19-graph-core/{baseline,candidate}/` |
| 安装 | `.install/pg19-pgorca-daft/` | `.install/pg19-graph-core/{baseline,candidate}/` |
| 数据与配置 | `.runtime/pg19-pgorca-daft/` | `.runtime/pg19-graph-core/{baseline,candidate}/` |
| 实例入口 | 55419 | 55449 / 55450，各自专属 Unix socket，TCP 关闭 |
| 回归端口 | 由旧集成脚本管理 | PG 55459 / 55460，AGE 55461 / 55462 |
| 加载方式 | 预加载 pg_orca，Daft Bridge 按需加载 | 三个 preload 设置均为空，测试会话显式 `LOAD 'age'` |

另有 AGE 属性统计实例 `.runtime/age-properties-stats-bench-20260902/`，端口 55439，也属于受保护的旧环境。本项目不使用它的库或数据。

### B-tree 专项 Release 变体（2026-09-26，D018）

新增 `btree-release` 用于四跳/100 行专项测量，不替换上表两套安装：

| 项 | 独立位置或要求 |
|---|---|
| 源码 | `.build/pg19-graph-core/btree-release/src/postgres19`，干净 detached `7ff975e259ce`，仅 C+B |
| 构建/安装/运行 | `.build`、`.install`、`.runtime` 下各自的 `pg19-graph-core/btree-release/` |
| 端口 | 实例 55471，PG 回归 55481；本轮图模块临时回归使用 55483 |
| 构建 | `--disable-debug --disable-cassert --enable-depend`，`-O2 -g` |
| 在线探针 | `LOAD 'test_graph_core'`，仍检查所有可连接库配置与真实映射；不安装/加载 AGE、adj、ORCA 或 Daft |

`lab.py` 对该变体增加封闭路径/端口和冻结源码检查；`isolation.py` 的在线探针只允许 age/test_graph_core 两个名称，不允许任意库绕过校验。新增错误端口、错误安装和错误源码三类拒绝测试，[自检证据](evidence/runs/20260926T095036.749671Z-package-selfcheck/summary.json)通过。通用 preflight 仍检查受保护原仓和 baseline/candidate；对 Release 必须显式指定 `lab.py isolation --variant btree-release`，在线检查加 `--online`。

Release 构建、图模块 9/9、PG 245/245 完成，现已停止专属实例，数据保留。[专项报告](golden/G1-080/图Btree四跳100行性能与10倍可行性评估.md)中的性能仍有同机换页污染；路径隔离不能替代资源隔离。未获得停服授权的 openGauss 55440 保持运行。

### B-tree 单因素优化对照（D019）

D020 同样受这些隔离规则约束：`btree-reuse` 在 `.build/.install/.runtime/pg19-graph-core/btree-reuse`，源码为 build 下 `src/postgres19`，端口 55473/55485，图模块临时回归 55484（与 opt 图回归必须串行）。它只重放 `btree_reuse_opt_v2.patch/json`，不改旧 release/opt/candidate；公共层兼容矩阵另用自有 `/tmp/pg19-graph-reuse-matrix-*` 克隆、临时实例 55486～55488，不安装到现有项目。性能与构建/回归必须串行。错误端口/安装/源码重定向均由自检拒绝。

D021 的 `btree-compact` 同样独立，位于 `.build/.install/.runtime/pg19-graph-core/btree-compact`，服务/PG 回归端口 55474/55489、图回归 55490；只接受 `btree_compact_opt_v2.patch/json`，旧 reuse 不变。路径/端口错指向 reuse/candidate 的拒绝测试已加入自检；不借用其他项目的库、实例或源码。组合矩阵复用上面的自有临时克隆前核对逐层 identity 和原补丁，原证据不覆盖。

D022 的 `btree-startup` 位于 `.build/.install/.runtime/pg19-graph-core/btree-startup`，服务/PG 回归 55475/55491、图回归 55492；纯 B 源只接受 `btree_startup_opt_v2.patch/json`，组合专属补丁不得进入它。旧 compact 作为同期对照保留；自检增加误用 compact 端口/安装与 candidate 源码的拒绝场景。实验继续使用独立 socket 与安装，不修改 ORCA/Daft/AGE 环境。

D023 的 `btree-deform` 位于 `.build/.install/.runtime/pg19-graph-core/btree-deform`，服务/PG 回归 55476/55493、图回归 55494；只接受 `btree_deform_opt_v1.patch/json` 的纯 C+B 源码，combined-only 补丁仅用于组合矩阵。旧 startup 保留同期对照；错用 startup 端口/安装或 candidate 源码均被拒绝，隔离负向用例累计 29 项。新增克隆须先核对并显式应用冻结补丁，再 build，不能删除 digest 校验。

D024 的 `btree-refview` 位于 `.build/.install/.runtime/pg19-graph-core/btree-refview`，服务/PG 回归 55477/55495、图回归 55496；只接受冻结 `btree_refview_opt_v3.patch/json`，旧 deform 保留同期对照。隔离自检增加错用 deform 端口/安装及 candidate 源码三项拒绝，累计 32 项。图实验服务均已停止，2026-09-28 恢复检查确认其他项目服务原状；不会用停止其他项目来改善本次基准。

D025 的 `btree-array` 位于 `.build/.install/.runtime/pg19-graph-core/btree-array`，服务/PG回归55478/55497、图回归55498；只接受 `btree_array_opt_v2.patch/json`。C+B 17文件和combined-only一文件分开；旧refview v3保留同期对照。隔离自检新增误用refview端口/安装与candidate源码的三项拒绝，累计35项通过。2026-09-29计时/采样完成后全部图服务停止，原有openGauss PID1638720不动，ORCA/Daft、原PG19/AGE与candidate安装未操作。矩阵下次从 `20260929T015556.610580Z-G1-080-btree-array-matrix` 的身份恢复。

新增封闭变体 `btree-opt`，使用 `.build/pg19-graph-core/btree-opt/src/postgres19` 的独立 C+B 克隆，以及 `.build`、`.install`、`.runtime` 下同名独立目录；实例/PG 回归端口分别为 55472/55482，图模块临时回归为 55484。它与原 btree-release 同为 `-O2 -g`、关闭 debug/cassert、只加载自身 test_graph_core，不安装 AGE/adj/ORCA/Daft。

唯一允许的 dirty 是 [btree_begin_opt_v1.json](golden/G1-080/btree_begin_opt_v1.json) 声明的四个 tracked 文件，逐文件 SHA 和整体 Git diff 身份写入构建及测量证据；HEAD 必须为 `7ff975e259ce`，不允许 untracked。`prepare` 可以创建尚未应用补丁的干净克隆，但 `build` 在缺少任何声明修改时拒绝。空克隆需应用[冻结补丁](golden/G1-080/btree_begin_opt_v1.patch)后再构建，不能任意复制开发工作树或伪造干净 Release。其端口/安装/源码重定向反例已加入自检。测量前校验实际安装哈希及源码身份；原 btree-release 和 candidate 安装不被覆盖。

候选目录名称虽然是 `.worktrees`，实际使用独立 Git clone：独立 `.git`，不借用原仓 objects alternates；初次 clone 使用 `--no-hardlinks`。AGE 15 项旧修改通过冻结 patch 重建。后续图内核修改只进入候选副本，基线与原仓保持为比较输入。

## 已接入的自动检查

[lab.py](scripts/lab.py) 调用 [isolation.py](scripts/isolation.py)，在 preflight、prepare、build/build-clean、start、regress 的相应入口检查：

1. 源码、构建、安装、运行和归档目录的真实路径，拒绝用软链指向其他位置；核对 Git common directory 与 objects alternates。
2. 安装树不得含 ORCA/Daft 文件，库软链不能越出本安装；禁止跨安装硬链接，但允许时区文件在同一安装内共享 inode。`pg_config` 的 bindir/libdir/pkglibdir/sharedir 必须属于当前变体。
3. 构建与客户端环境清除调用者的 `PG*`、`LD_*`、`GIT_*`、编译器/头文件/链接搜索变量、`DESTDIR` 和 Make 注入变量；PATH 固定为 `/usr/bin:/bin`，AGE 使用显式的本项目 `PG_CONFIG`。需要变更工具链时，应更新冻结配置并复验，不能靠终端环境悄悄切换。
4. 启动前通过 `postgres -C` 读取生效配置，包括 include 和 `postgresql.auto.conf`，核对三个 preload、`dynamic_library_path`、PGDATA、配置文件、socket 和端口。发现混入即失败，不自动改写旧配置。
5. 启动后只读检查所有可连接数据库的扩展、ORCA/Daft GUC、C 函数库引用、数据库/角色配置及配置 include 路径；新建 AGE 探针会话（btree-release 改用 test_graph_core），检查 `/proc/self/maps` 中真实的 PG/探针库文件路径及是否映射 ORCA/Daft。

检查失败会阻止后续实验操作；已启动的本项目实例仍可通过专属 stop 入口停止。检查防止约定流程里的串用，不是阻止同一 OS 用户或数据库超级用户手工改路径、执行 `LOAD '/绝对路径/库'` 的权限沙箱。开发和验证应持续走本执行包入口。

## GLM-5 的复验命令

```bash
cd /home/qiumc/data/code/cpp
P='公共文档/PostgreSQL19原生多跳图执行与AGE适配/执行包'
python3 "$P/scripts/lab.py" preflight
python3 "$P/scripts/lab.py" isolation --variant candidate
python3 "$P/scripts/lab.py" start --variant candidate
python3 "$P/scripts/lab.py" isolation --variant candidate --online
python3 "$P/scripts/lab.py" stop --variant candidate
```

`isolation` 默认只做离线检查，不启动数据库；尚未准备/安装的全新环境会明确给出 PARTIAL/NOT_BUILT，不能当成完整验证。`--online` 要求实例已经启动并具有本地 socket 权限，结果保存在 `evidence/runs/*-isolation-*/summary.json`。baseline 使用同一命令替换 variant。`start` 自身也强制执行在线检查，无须依赖人工记住额外命令。

## 本次证据（2026-09-06）

| 验证 | 结果与证据 |
|---|---|
| baseline 在线隔离 | [PASS，配置/扩展/实际映射库](evidence/runs/20260906T083938.109912Z-isolation-baseline-online/summary.json) |
| candidate 在线隔离 | [PASS，配置/扩展/实际映射库](evidence/runs/20260906T083954.752574Z-isolation-candidate-online/summary.json) |
| 错配拒绝 | [11 类反例通过](evidence/runs/20260906T083859.726526Z-package-selfcheck/summary.json)，含真实 `postgres -C` 对 include/auto.conf 的读取 |
| 接入后双前端差分 | [baseline 128/128](evidence/runs/20260906T083742.044121Z-baseline-differential-baseline/summary.json)、[candidate 128/128](evidence/runs/20260906T083819.099405Z-baseline-differential-candidate/summary.json) |
| 旧环境前后核对 | [比较摘要](evidence/runs/20260906T083316.340911Z-isolation-verification/summary.json)：四个原仓的 HEAD、tracked diff 和 git status 摘要，以及清单内旧二进制/配置哈希一致 |

本次没有重建旧集成、重启旧库、运行旧项目的任务协议或修改旧源码。两套图实验库验证后恢复停止状态，数据保留。现有 AGE 六项基线差异、业务输入和图内核任务状态保持原有结论。

## 性能测试与将来合并

路径和进程隔离不隔离同机 CPU、内存、页缓存、磁盘/WAL 带宽。当前报告明确标记 `resource_isolation=NOT_ENFORCED`，没有为本项目建立 cgroup 或独立虚拟机。

日常功能开发可与旧集成共存。正式性能验收须在预先冻结的资源条件下运行：选择无竞争的独占测试时段或独立主机/虚拟机；共机时配置并记录 CPU、内存和 I/O 配额，采集竞争负载，基线与候选使用同样条件。WSL 的宿主机负载也应纳入记录。存在未控制的竞争时，性能数据只能用于开发诊断，不能证明 DML 零退化。执行包不自动停掉其他项目来抢占资源。

若以后要求“新图内核 + ORCA/Daft”共同工作，另建兼容实验环境：用修改后的 PG19 头文件重新构建 AGE、pg_orca 及其 Bridge，验证新 NodeTag/Query/Plan 字段与扩展 ABI、planner hook 接管/回退、新图节点和 DaftExec 边界、MVCC/RLS、prepared plan，以及各自原回归。ORCA 成功生成计划时不继续调用之前的 planner hook，这一点尤其需要适配并验证。不能直接把旧 `.so` 拷入新内核，也不能用本次隔离 PASS 推导两套功能已兼容。
