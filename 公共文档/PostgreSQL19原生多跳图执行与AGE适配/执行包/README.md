# PG19 原生多跳图执行与 AGE 适配执行包

将 [MODEL_HANDOFF_PROMPT.md](MODEL_HANDOFF_PROMPT.md) 交给能访问本工作区并执行终端命令的 GLM-5。它是唯一启动入口；任务进度以状态工具和真实证据为准。

原架构文档保留在原位置：[02. PostgreSQL19 原生多跳图执行与 AGE 适配详细设计](../../02.%20PostgreSQL19原生多跳图执行与AGE适配详细设计.md)。本包补充实施，不复制原设计或改造另一个 ORCA/Daft 项目。

## 已准备与尚未交付

2026-10-21 当前断点：M0～M6本轮实施、回归与限定评估已结束，按约定停止，见[最终报告](../报告/图Btree组合执行M0-M6验收报告.md)及[勾选清单](../设计/图Btree与关系算子组合执行设计.md#5-实施清单与停止条件)。正式内核入口无需LOAD模块；功能、兼容和四组合各PG245通过。新增性能矩阵5/7达三干净轮，历史四场景复测均未达环境门，自然CBO准确性/DML零退化未签收，不宣称生产准入。PG25项tracked＋16个新文件未提交，AGE未改；G1-080仍in_progress、默认AUTO/AGEoff，所有自有实例已停。精确身份见[交接入口顶部](MODEL_HANDOFF_PROMPT.md)，下文D034等为历史记录。

最新D034：已按[详细执行任务](golden/G1-080/共享前缀执行器优化任务与验收.md)完成采样、属性后缀复用实现、四组合及语义验证、三轮同期216窗口测试；因主目标29→28μs仅1.036×、未达预设5%增量门且存在污染，原44边33→33无收益，**候选未接受并已精确撤回**。补丁/独立安装/全部证据保留；宽属性33→29μs只是观察，不签收稳定改善或10×。主树回到anchor v1的27项dirty，D033统计9文件仍未验收，矩阵仍suffix v1。详见[最新交接](继续开发交接提示词.md)、[报告§22](golden/G1-080/图Btree四跳100行性能与10倍可行性评估.md)。G1-080不complete/amend，以下为历史。

最新D032：未改内核，完成8个新非死路案例、完整四跳100行的同二进制三轮对比。早期平行边NL/普通图/共享前缀170/49/28μs，18/18干净、6.071×；宽属性平行边142/59/33μs、4.303×，同样18/18干净。仍未达10×，其余6例有污染；全部144窗口/614743样本保留。现有全图统计不能区分本次8个条件起点的工作量，下一步先补anchor分层/覆盖和DAG成本输入，自动选择尚未实现。主树/矩阵仍factor v3，图服务全停，默认关闭、G1-080未完成。见[最新交接](继续开发交接提示词.md)及[报告§21](golden/G1-080/图Btree四跳100行性能与10倍可行性评估.md)，不要将新场景成绩当成本轮代码收益。

上一轮D031已实现并测量：多端点批恢复后，原小图/树形/宽属性上版→本版36→31、101→64、40→34μs；同期NL/本版2.548/1.406/2.412×。新平行死路观察185.862×，不替代原主例。三轮150窗口98干净，正式五例均INCONCLUSIVE；四组合回归、128组原生语义、75组批量边界通过。主树冻结factor v3、btree-factor-batch（55504），三个图实验服务停止，默认未开启、不amend或complete G1。精确恢复与下一成本选择断点见[交接顶部](继续开发交接提示词.md)及[报告§20](golden/G1-080/图Btree四跳100行性能与10倍可行性评估.md)。以下为历史。

历史D029（2026-10-01）：通过等价SQL先验证前瞻裁剪，新增死路场景仍完整四跳100行、辅助计算全计入执行；中间路径110180→80，NL/裁剪观察12.831/0.210ms、61.1×，但计时污染正式INCONCLUSIVE。不是图B-tree内核增益，也不替代原44边主例；原主例无可裁死路、裁剪更慢。当时尚未实施原生方案，详见[报告§18](golden/G1-080/图Btree四跳100行性能与10倍可行性评估.md)。

最新D028（2026-09-30已测、2026-10-01恢复）：完成路径分解参考证明和死路诊断，405组完整bag对照PASS；新四跳254边仍完整100行，当前110180条中间父链可由250条中间关系弧表示，CPU父链/父批初始化共76.50%。这是结构和热点证据，不是原生实现或440倍加速；新计时有污染、INCONCLUSIVE，原44边主例最新干净结论仍108/45 μs、2.40×、10× FAIL。下一项先设计有界分解/逐行枚举的资格与语义门，尚未接入内核。主树/矩阵身份和其他项目未改，图服务全停，G1-080仍in_progress。详见[报告§17](golden/G1-080/图Btree四跳100行性能与10倍可行性评估.md)及[交接顶部](继续开发交接提示词.md)。以下为历史。

最新 D027（2026-09-30）：D026直传候选宽属性干净三轮慢16.7%，已撤回；主树精确恢复array v2的18文件、1014新增/120删除，未提交。D027固定开销诊断36/36窗口干净，原四跳完整100行NL/图108/45 μs、约2.40×、10× FAIL；空输入7 μs只作诊断。全部图服务已停，其他项目原状；矩阵仍tail v2、主树array v2，身份不可混用，G1-080仍in_progress。证据/下一断点见[报告§15～16](golden/G1-080/图Btree四跳100行性能与10倍可行性评估.md)及[交接顶部](继续开发交接提示词.md)。以下为历史记录。

最新 D025（2026-09-29）：原生 B-tree 等值数组批扫描通过四组合、完整语义及并发/SSI 检查。主例/树形/宽属性同期旧图与新图 40/39、87/77、56/60 μs，新 NL/图 2.462/1.377/1.567×，干净窗口 16/24、14/24、18/24；正式均 INCONCLUSIVE，未达 10×，稳定收益未签收。最新冻结 array v2、火焰图及下一断点见[报告 §14](golden/G1-080/图Btree四跳100行性能与10倍可行性评估.md)与[交接顶部](继续开发交接提示词.md)。主树 18 项 tracked dirty、1014 行新增/120 行删除，未提交；全部图服务已停，其他项目原状，G1-080 仍 in_progress。以下为历史记录。

最新 D023（2026-09-27）：共享候选 opposite key 解码复用通过四组合与语义/并发检查，但未证实稳定增量收益。三轮主例/树形旧图与新图为 40/40、85/85 μs，各 24/24 窗口干净；新 NL/图为 2.375×、1.247×，10× 目标 FAIL。宽属性 55/56 μs、新 NL/图 1.679×，22/24 窗口干净，INCONCLUSIVE。新 deform 火焰图、复跑及后续断点见[报告 §12](golden/G1-080/图Btree四跳100行性能与10倍可行性评估.md)与[交接顶部](继续开发交接提示词.md)。主树 17 项 tracked 修改、未提交，C+B 16 文件与 combined-only 1 文件补丁分开；候选保留不代表性能签收。所有图实验服务停止，原 candidate、旧实验版本及其他项目不变。G1-080 仍 in_progress，不跨历史轮次拼接最优值。

2026-09-26 当前断点：`G1-080 in_progress`，完整 G1/Phase 2 未通过。C/B/A 已按 D017 独立解耦；最新专项为[图 B-tree 四跳、完整输出 100 行的 10× 评估](golden/G1-080/图Btree四跳100行性能与10倍可行性评估.md)。独立 C+B Release 三类观察倍率为 1.57×/1.00×/1.47×，未达目标，换页污染使正式判定为 INCONCLUSIVE；B-tree 自身火焰图、阶段计时、9/9 图回归和 245/245 PG 回归已保存。`btree-release` 实例已停止，原 candidate 未替换，其他项目保持隔离。下面 2026-09-18 的能力数量是历史快照，当前以[交接顶部](继续开发交接提示词.md)与任务账本为准。

2026-09-08 已按最新主设计刷新为执行包 V2：稳定 Endpoint Slot/VertexRef、紧凑小邻接、两层有界批及第 8 章统计实现链。现有 **72 张任务卡**。截至 2026-09-18 17:00，18 项 completed、52 项 todo、2 项 deferred，active task 为空；最新动态状态见 [当前实现交接](继续开发交接提示词.md)。

已提供冻结的 commit/AGE dirty patch、隔离源码/构建/运行入口、当前参考契约、状态与证据工具、小图 oracle、双前端差分、开发基准及性能实验统计分析器。图统计目录/codec/维护 API 已由 G1-015 实现；真实采集、planner 消费和校准尚未完成，不能与 performance.py 性能分析器混为已实现统计闭环。

实现前基线已实测 PG19 原有回归 245/245、16 个手工 oracle、128 个固定多跳差分；开发基准完成 3 轮×读/写×1/4 客户端。G0-035 已逐项关闭 AGE 的 6 项 PG19 差异，候选 AGE 回归现为 43/43。

候选副本已实现并正式验证 GS-010～070：`graph_adj` AM、双向 WAL 追加、普通 OUT/IN 扫描与 MVCC/HOT、稳定 ref 直达、目录分裂、Base/Delta 发布、紧凑共享小段、callback VACUUM、WAL free-list、普通 Index Scan 的 SSI 与故障注入恢复。G1-010 已完成公共 fixed 节点/生成及 ABI 契约，G1-015 已完成独立图统计目录与原子发布，G1-030 已完成普通非覆盖 B-tree 的有界邻接候选 Provider。最新 Provider 证据为 `evidence/runs/20260918T085953.726281Z-task-G1-030/evidence.json`，原生模块回归 1/1。完整候选 PG/AGE 回归分别通过 245/245、43/43；实验实例已停止，数据与二进制保留。

当前 `graph_adj` 仍是有界存储原型：单段洞/Slot 复用、额外 Slot extent、多段 Base 等尚未完成。公共 rewrite、graph_adj/AGE Provider 接入、新执行节点、两层批回表、统计采集与消费校准、自然 CBO、AGE adapter 均未完成；正式业务 SLO/负载也未提供。因此 Phase 2、正式 G0/GS 门和生产准入均未完成。图统计不修改 PG 原生 ANALYZE/列与扩展统计/pgstat/AGE 统计入口；初版维护/raw API superuser-only，RLS 全局 profile 拒绝。

## 文件导航

| 文件 | 用途 |
|---|---|
| [图 B-tree 四跳/100 行专项评估](golden/G1-080/图Btree四跳100行性能与10倍可行性评估.md) | 当前 10× 目标、Release 结果、真实 B-tree 火焰图、测量限制及后续实验（D018） |
| [代码分层与独立迁移说明](代码分层与独立迁移说明.md) | 最新 C/B/A 三提交、独立迁移命令、四组合验证及重编要求（D017） |
| [实施契约.md](实施契约.md) | 实现边界、内存/ABI/节点生成、语义和证据要求 |
| [与 pg_orca 及 Daft 的开发隔离](与pg_orca及Daft的开发隔离.md) | 路径/加载自动检查、实际在线证据、资源与未来兼容边界 |
| [任务导航.md](任务导航.md) | 72 张任务卡与统计/批执行前置链；具体定义以 JSON 为准 |
| [tasks.json](specs/tasks.json) | 原子任务依赖、阅读路线、修改范围、assertions、验证命令 |
| [task_state.json](state/task_state.json) | 当前状态，只通过 taskctl 修改 |
| [source_lock.json](specs/source_lock.json) | 双仓源码锁，AGE 补丁摘要及构建参数 |
| [acceptance.json](specs/acceptance.json) | 已命名的开发配置、正式业务待填字段和零退化规则 |
| [业务验收输入填写说明](specs/业务验收输入填写说明.md) | 12 个待填字段的含义、来源和整理方式 |
| [V2 基线绑定](specs/design_baseline_v2.json) | 主设计摘要、活动参考契约、必须存在的断言/依赖；检查设计漂移 |
| [V2 存储契约](specs/graph_adj_prototype_v2.md) / [批执行](specs/batch_execution_v2.md) / [统计首版](specs/graph_statistics_v1.md) | 稳定 ref、紧凑共享段、回表批和统计实现协议 |
| [Provider V2](include/graph_provider_v2.reference.h) / [fixed IR](include/graph_program_v2.reference.h) / [页字段 V2](include/graph_adj_page_v2.reference.h) / [统计](include/graph_stats_v1.reference.h) | 活动参考头；Catalog/Result ABI 仍 V1，准确实现以候选头/codec 为准；参考存在不代表 Provider、执行器或采集器已交付 |
| [small_graphs.json](golden/small_graphs.json) | 固定手工预期及反例 |
| [checks 说明](checks/README.md) | 后续各任务真实验证程序的输出契约 |
| [决定记录](decisions/DECISION_LOG.md) | 开发选择、业务输入和范围变化的记录 |
| [历史准备验证报告](准备验证报告.md) | 2026-09-06 实际命令、结果、缺口；原位保留，不代表当前任务总数 |
| [V2 离线检查入口](scripts/rebaseline_check.py) | 包/设计一致性、自检、源码预检与 ready 状态；保存独立日志，不启库、不完成任务 |

旧 `graph_adj_prototype_v1.md` 与三个 V1 参考头原位保留，仅服务历史证据；活动任务不得再按“leaf 内嵌 Descriptor/16 字节 entry/每 hop 查目录”实施。历史 completed 卡保留原定义用于日志摘要核验。

## 最短入口

以下命令中的 P 是执行包路径；全部脚本使用 Python 标准库，构建沿用本机工具链，无需联网安装依赖。

```bash
cd /home/qiumc/data/code/cpp
P='公共文档/PostgreSQL19原生多跳图执行与AGE适配/执行包'
python3 "$P/scripts/validate_package.py"
python3 "$P/scripts/lab.py" preflight
python3 "$P/scripts/taskctl.py" list-ready
python3 "$P/scripts/taskctl.py" show G1-080
```

`list-ready` 才是当时可领取任务的依据。当前先核验并恢复 G1-080，不按上面历史快照重做 G1-025/035/020；同一时间只能领取一个任务。正式输入 G0-060 未填写时只阻塞依赖该输入的正式验收，不能由模型臆造。

G1-050 自然成本竞争现在依赖 G1-048 最小统计/校准；G1-080 还必须经过 G1-035 两层批、G1-055 稳定 ref 接入和 G1-075 联合测试。G2-040 只增加自动刷新/长路径外推，不再承担首版全部统计。不得按旧阶段顺序跳过这些新增前置。

## 隔离构建和回归

```bash
python3 "$P/scripts/lab.py" prepare --variant candidate
python3 "$P/scripts/lab.py" build --variant candidate --jobs 4
# 阶段门或首次全量验收使用：
# python3 "$P/scripts/lab.py" build-clean --variant candidate --jobs 4
python3 "$P/scripts/lab.py" start --variant candidate
python3 "$P/scripts/check_baseline.py" --variant candidate
python3 "$P/scripts/lab.py" regress --variant candidate --suite pg
python3 "$P/scripts/lab.py" regress --variant candidate --suite age
python3 "$P/scripts/lab.py" stop --variant candidate
```

baseline 也可使用同一组命令；首次构建已经完成。`build` 复用专属目录作增量构建，开启 PG `--enable-depend`；`build-clean` 要求先停止本实例，将已识别的旧 build/install 移入专属 `_archive`，然后从空目录构建，AGE 使用其 PGXS clean 目标清理生成对象。它不删除原始仓库或候选源码，也不自动删除备份。失败时保留备份与本轮日志，检查后再恢复，不能用部分安装启动数据库。最终阶段门使用 build-clean 并重跑当前实现全部必需测试。

本实例只监听 Unix socket。baseline/candidate 服务端口标识分别 55449/55450；PG 回归端口 55459/55460；AGE 回归端口 55461/55462。socket 创建/连接在受限 Codex 沙箱里会报 Operation not permitted，需要按平台工具请求授权；这不是数据库测试失败。工具已有授权不应重复请求相同动作。

preflight/构建/启动/回归已接入隔离检查，启动还核对实际数据库配置与 AGE 会话映射库；路径串用、ORCA/Daft 库或预加载配置混入会报 `ISOLATION` 并停止后续实验。可运行 `lab.py isolation --variant candidate` 保存离线报告，已启动实例加 `--online`。细节和双实例 PASS 证据见[隔离说明](与pg_orca及Daft的开发隔离.md)。同机 CPU/内存/I/O 尚未隔离，正式性能验收另需控制竞争负载。

AGE 回归入口显式传 `--dbname=contrib_regression`。缺少它会导致 scan 用例的 `\c contrib_regression` 失败并污染后续结果；不能拿早期 10 项失败当最终基线。G0-035 的当前候选结果是 43/43，证据见当前交接文档。

`check_baseline.py` 和 `benchmark.py` 只在它们自己创建的 `graph_lab` 库内重建 `glab_*`/`age_glab_*` 实验对象，不接收外部连接参数。长回归实例及构建产物归 `.runtime/.build/.install`，日志与结果归本专题树。

## 任务推进

```bash
python3 "$P/scripts/taskctl.py" show G1-025
python3 "$P/scripts/taskctl.py" claim G1-025 --actor <模型名>
# 完整阅读该任务，先补独立验证，再实施任务范围内的变更。
python3 "$P/scripts/taskctl.py" run G1-025
python3 "$P/scripts/taskctl.py" complete G1-025
python3 "$P/scripts/taskctl.py" list-ready
```

GS-060/070 与 G1-010/015/030 的 checks 已实测并关闭。G1-030 通过实际 SQL、原生模块回归及并发快照/取消/SSI 测试验证，不只是 ABI 参考头。G1-025 的 check/采集器仍未实现，run 返回 NOT_IMPLEMENTED；须交付真实采集与 maintenance/statistics evidence，不能再用 synthetic codec fixture 冒充。完整 G1 的自然成本竞争仍是后续门。

遇到实质阻塞时用 `block <ID> --reason '具体证据与缺少的输入'`；后续恢复用 `reopen <ID> --reason '解除条件或重做原因'`。不要重跑 init 清空历史，不要手工把状态改为 completed。

维护执行包增加任务时使用 `taskctl.py sync-definitions --reason '具体设计决定'`：只追加 todo/deferred，拒绝删除历史任务、改写 completed 定义或在活动任务期间同步；不会自动完成新任务。本次 V2 已同步，正常接手无需再运行它。
