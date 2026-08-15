# MiniCyber 第二次重构路线图与 Agent 行为基线

> 本文件是新会话和接力 Agent 的第一入口。当前实施范围从 **MC-601** 开始；MC-001 至 MC-504 仅作为第一次重构历史，不得用其旧结论覆盖本轮基线。

## 一、当前指针

- 当前分支：`development`
- 当前阶段：第二次重构，业务模块改造阶段
- 当前任务：MC-617
- 当前状态：待开始
- 唯一状态事实源：[`docs/refactor/00_进度记录.md`](docs/refactor/00_进度记录.md)
- 唯一任务执行方案：[`docs/refactor/01_实施方案.md`](docs/refactor/01_实施方案.md)

收到“执行 MC-601”或其他指定任务时，Agent 不需要重新向用户确认已经在基线中定稿的需求；应按本文阅读顺序取得上下文，核对进度后独立推进。只有源码事实与基线互相冲突、外部依赖缺失或任务会越过白名单时，才允许标记阻塞并提问。

### 1.1 MC-601 至 MC-606 已交付基线

- 根级 `AGENTS.md` 已建立，后续会话必须先读取其中十条红线。
- MC-601 至 MC-603 已完成接力基线、知识迁移和白名单外物理清理。
- MC-604 已完成显式生产/Proto/测试源列表、Scheduler 配置骨架及系统 FastRTPS
  必需依赖门禁；当前系统实际导出 target 为 `fastrtps`。
- MC-605 已完成 `time::Time`、`time::Duration`、`time::Rate` 职责分离。
- MC-606 已完成 FastRTPS Channel Join/Leave 控制面、原生字段编号、独立
  `ChannelManager` 状态层、控制 Topic 原生 QoS、回环去重和确定性跨进程测试。
- MC-603 已完成 RPC、TimerComponent、旧示例、旧 benchmark、旧脚本及其专属测试的物理删除；
  Debug 重新配置、全量构建和黑名单扫描均通过。当前精确留存清单记录在
  `docs/refactor/00_进度记录.md`，是 MC-604 显式源列表的唯一输入。
- MC-607 已接收发现快照并恢复 INTRA/SHM HybridTransport；MC-608 已将 Hybrid 接入
  Protobuf-only Node API、RoleAttributes、`HasReader` 和端点生命周期；MC-609 已恢复
  单/双输入 DataVisitor、AllLatest 和 RoutineFactory 数据协程链；MC-610 已恢复
  Classic 共享调度组并接回 DataVisitorBase 唤醒；MC-611 已完成 Choreography 定向
  Processor、Classic 公共池、任务回退和原路由唤醒；MC-612 已将单/双通道 Component
  接入 DataVisitor、RoutineFactory 和 Scheduler，补齐回调内关闭与 SHM 数据协程分流；
  MC-613 已完成独立 Scheduler 配置、真实 `.so` 加载水位回滚和主线程有序退出；
  MC-614 已定义业务 Protobuf，固定六类消息的源序列/单调时间字段契约并完成生成验证；
  MC-615 已完成同一业务 `.so` 内五组件、唯一 DAG、真实 TextFormat 加载、确定性链路和
  `dlclose` 注册注销验证；MC-616 已完成 Source/Sink、拓扑 Join 放行、统一脚本、指标和
  有序退出。下一步为 MC-617。
- MC-602 必须先从 `docs/refactor/baseline.md`、`docs/refactor/module_mapping.md`、
  `docs/refactor/perf/**`、`docs/croutine/shared_from_this.md`、
  `docs/scheduler/debug_vtable_hang.md`、`docs/transport/signal.md` 和
  `docs/uml/**` 迁移仍有效事实到双手册，再删除这些离散资料与旧性能原始数据。
  MC-603 已按任务卡完成物理删除；MC-604 只接收精简后的工程，不得恢复已删除能力。

### 1.2 MC-602 文档迁移

- `docs/DEBUG_MANUAL.md` 与 `docs/INTERVIEW_QA.md` 已建立初稿，保留协程 ABI 与
  栈冻结引用、Notifier/AllLatest、SHM 信号清理、调度关闭、动态加载以及关键取舍。
- 首轮离散笔记、UML、旧性能报告与原始 CSV/JSON 已在迁移后删除；旧性能数值没有
  进入新手册。最终性能证据只允许由 MC-620 的唯一业务主干生成。
- MC-603 接收的是已收敛的文档体系；它只清理代码、测试、旧示例与构建入口，不得
  回填或恢复本任务删除的离散资料。

## 二、强制阅读顺序

### 2.1 新会话或上下文丢失

1. `MINICYBER_ROADMAP.md`：读取红线、范围、文档职责和当前入口。
2. `docs/refactor/00_进度记录.md`：确认唯一当前任务、状态、前序 Commit 和阻塞项。
3. `docs/refactor/01_实施方案.md`：只读取当前任务卡、全局白名单/黑名单和共同门禁。
4. `docs/refactor/02_架构取舍矩阵.md`：读取当前任务涉及的已定稿架构决策。
5. `docs/refactor/03_验收与性能口径.md`：读取当前任务必须满足的验证口径。
6. `docs/refactor/04_自校验定稿.md`：确认该任务与上下游任务没有需求断链。
7. `docs/refactor/05_实施归档.md`：读取第一次重构可复用事实、证据规则和接力格式。
8. 当前任务卡列出的 `cyber_ref` 与 MiniCyber 精确源码文件。
9. MC-601 创建根目录 `AGENTS.md` 后，后续会话必须在第 1 步之前先读取它。

### 2.2 文档职责与冲突处理

| 文档 | 唯一职责 | 发生冲突时的处理 |
|---|---|---|
| `00_进度记录.md` | 当前任务、状态、Commit、验证结果 | 状态以 00 为准，不得从 Git 猜测任务状态。 |
| `01_实施方案.md` | MC-601~MC-623 的执行顺序、输入、设计和产出 | 实施范围以当前任务卡为准，不得跨卡修改。 |
| `02_架构取舍矩阵.md` | 已由用户确认的保留、删除和实现边界 | 不得自行改选型；新冲突必须先暂停并登记。 |
| `03_验收与性能口径.md` | 功能、测试、覆盖率、性能和退出标准 | 不得用额外测试替代任务卡要求的主验收。 |
| `04_自校验定稿.md` | 跨文档一致性、决策覆盖和变更门禁 | 发现断链时不得开始产品代码修改。 |
| `05_实施归档.md` | 历史事实、证据索引和会话交接 | 历史实现不代表当前目标，旧结论不得覆盖 02。 |

若七份基线文件出现实质冲突，Agent 必须先在 `00_进度记录.md` 标记阻塞并给出冲突路径，不得选择对自己实现最方便的版本。

## 三、绝对行为红线

1. **全中文文档**：除代码、CMake 官方指令和英文专有名词外，所有 Markdown 必须使用中文。
2. **CyberRT 忠实度**：禁止自创优化或伪装原生能力。每个新增职责必须能指向 `cyber_ref` 对应设计；项目删减可以更窄，但不能改变所保留职责的语义。
3. **双手册同步**：重大 Bug、泄漏、死锁、ABI、生命周期和选型必须在同一任务同步沉淀至 `docs/DEBUG_MANUAL.md` 和/或 `docs/INTERVIEW_QA.md`。
4. **单任务单提交**：一个 MC 任务对应一个 Commit。不得跳过、合并或在一个提交夹带下一任务；用户再次明确授权除外。
5. **分支限制**：所有提交和 Push 仅允许进入 `development`，严禁直接提交或推送 `main`。
6. **适度验证**：保留高风险单测与完整业务集成测试，不为追求测试数量恢复无业务价值的孤立测试。
7. **台账纪律**：开始任务前先把 00 中该任务标为“进行中”；完成后填写真实验证与 Commit，再标为“已完成”。
8. **工作区保护**：现有未提交修改默认属于用户；只按路径暂存当前任务文件，不得回退或覆盖无关修改。
9. **不重复澄清**：02/03 已明确的决策不再询问用户。只有新事实导致不可兼容的二选一时才提问。
10. **意图注释门禁**：新增或实质改动的公开 API、协议字段、并发状态机、资源所有权、
    失败回滚和关闭顺序必须写中文意图注释，说明关键不变量和原生职责位置；注释解释
    “为什么”和边界，不逐行复述语法。
11. **知识增量门禁**：每个生产代码任务必须在同一 Commit 增量更新
    `INTERVIEW_QA.md`；重大排错还要以完整时间线更新 `DEBUG_MANUAL.md`，不得等待
    MC-621/MC-622 集中补写。
12. **联动收口门禁**：每任务结束必须同步 Roadmap、00、04、05 的任务指针和产物状态；
    01/02/03 即使无需修改，也要在 00 记录逐份检查结果。MC 生产改动仍保持一个任务一个
    Commit；因 Commit 无法在自身内容中记录最终 SHA，Push 后允许追加不带 MC 编号的纯台账
    证据 Commit，只补录完整 SHA、subject 和远端状态，严禁夹带生产代码或下一任务改动。

### 3.1 调试记录最低结构

重大问题条目必须依次包含：触发环境与最小复现、原始现象、按时间排序的排查步骤、
候选假设及排除证据、实际工具命令、关键输出、证据结论、根因、修复边界、回归结果和
可迁移经验。只记录“根因 + 修复”不满足交付要求。

并发、跨进程、调度或生命周期测试只要出现一次非预期失败，即使随后复跑通过，也必须
建立“未确认问题”条目，记录命令、退出码、失败率、关键输出和后续归属任务；不得只用
最后一次通过覆盖已经观察到的异常。

## 四、项目终局定位

MiniCyber 是基于 Apollo CyberRT 源码进行深度精简的 C++17 中间件实践项目。终局只保留并讲清以下主干：

- x86_64 栈式用户态协程和 `DATA_WAIT -> READY -> SwapContext` 数据驱动调度；
- Classic 调度组共享 20 级优先级队列；
- Choreography 定向 Processor 与 Classic 公共池双区；
- Node/Reader/Writer 的 Protobuf-only Channel API；
- 同进程 INTRA shared_ptr 零拷贝；
- 跨进程 POSIX SHM Protobuf 序列化传输；
- FastRTPS 仅承担同机 Channel Join/Leave 拓扑发现控制面；
- HybridTransport 按具体对端关系同时扇出 INTRA 和 SHM；
- 单/双通道 Component、AllLatest、`.dag`、`.so`、dlopen 与 mainboard；
- 唯一自动驾驶业务流水线、业务视角集成测试和同链路性能数据。

严格平台边界为 `Linux x86_64 + C++17 + POSIX SHM`。跨主机 RTPS 数据、RPC、Timer/TimerComponent、监控、参数、录包回放、Python、旧 benchmark 和离散 Demo 均不属于终局能力。

## 五、唯一自动驾驶主干

```text
SensorSource（独立进程，time::Rate 控制输入节拍）
  -> CameraFrame + VehicleState（跨进程 SHM）
  -> PerceptionComponent<CameraFrame>
  -> PerceptionObstacle（同进程 INTRA）
  -> FusionComponent<PerceptionObstacle, VehicleState>（AllLatest）
  -> FusedObstacle（同进程 INTRA）
  -> PlanningComponent<FusedObstacle>
  -> Trajectory（同进程 INTRA）
  -> ControlComponent<Trajectory>
  -> ControlCommand
       -> ControlAuditComponent（同进程 INTRA）
       -> ControlSink（跨进程 SHM）
```

业务组件编译为同一个 `.so`，由同一份 `.dag` 经 ModuleController 强制 dlopen。Classic 和 Choreography 只替换 Scheduler 配置，不复制业务 DAG。默认验收为 `--messages 1000 --frequency 100 --metrics`。

## 六、任务路线

| 阶段 | 任务 | 目标 |
|---|---|---|
| 准备阶段 | MC-601~MC-603 | 固化 Agent 红线、迁移历史知识、删除白名单外文件。 |
| 基础设施改造 | MC-604~MC-613 | 重建构建、时间、发现、Hybrid、Node、数据协程链、两种 Scheduler、Component 和 mainboard。 |
| 业务模块改造 | MC-614~MC-616 | 完成业务 Protobuf、组件 `.so`、唯一 DAG、SensorSource、ControlSink 和统一启动。 |
| 联调测试 | MC-617~MC-620 | 收敛高风险单测，完成双策略业务验收、文件级触达和新性能采集。 |
| 收尾验收 | MC-621~MC-623 | 完成架构、调用链、调试、面试文档，最终验证并推送 development。 |

不得直接执行整个阶段。每次只执行 00 指向的一个任务卡。

MC-613 质量评估确认的局部门禁已分别归入 MC-615 的配置加载、
MC-617 的信号/唤醒/绑核/资源测试和 MC-621 的注释一致性审计。
MC-616 交付评估的细节门禁已下沉到 MC-617~620：指标关闭零样本开销、
SHM 成功路径自然回收、Control/Audit 集合与 Hybrid 扇出证据、丢包/重复/乱序独立统计。
这些都是局部任务验收，不新增根级红线。
接力 Agent 只在执行对应任务时读取具体门禁，不将这些实现细节追加为新的全局红线。

## 七、固定接力指令

新会话可直接使用以下指令：

```text
读取根目录 AGENTS.md（若已存在）、MINICYBER_ROADMAP.md 和
docs/refactor/00_进度记录.md；再按路线图规定的顺序读取 01 至 05。
从 00 指向的当前任务继续，严格执行当前任务卡，不跳过、不合并、不重复
询问已经在 02/03 定稿的决策。开始前更新台账为进行中，完成后执行任务卡
规定的最小验证、更新双手册（如适用）、创建单一 Commit 并推送 development。
```
