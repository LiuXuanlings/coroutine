# MiniCyber 自顶向下调用链

## 一、阅读约定

本手册以唯一自动驾驶主干的一次完整运行为线索。每层先给出实际函数，
再给出源码路径。Classic 和 Choreography 只在 Scheduler 路由点分开，
前后的 DAG、Component、Channel 和业务数据链完全相同。

## 二、三进程启动与就绪

### 2.1 启动脚本

`scripts/run_autodrive_pipeline.sh` 按以下顺序管理进程：

1. 启动 `control_sink`，创建 `/autodrive/control_command` 和
   `/autodrive/control_audit` 两个 Reader；
2. 启动 `mainboard -d config/autodrive/autodrive.dag -s <scheduler.conf>`；
3. Sink 通过 `Reader::HasWriter()` 观察 Control Writer Join，写入单向 ready 文件；
4. 启动 `sensor_source --messages --frequency --ready-timeout-ms --sink-warmup-file --await-shutdown`；
5. Source 通过两个 `Writer::HasReader()` 等待 Camera/VehicleState Reader Join；
6. Source 有界重发 sequence 0，直到本地 Audit 和跨进程 Sink 都回执，再发布测量序列；
7. Sink 收齐 Control/Audit 序列后退出，脚本用 SIGTERM 依次请求 Source 和 mainboard 关闭。

这里的 `sleep 0.01` 只是对 ready 文件的有界轮询间隔，不是用固定延时
猜测拓扑已就绪。

### 2.2 mainboard 参数与 Scheduler

`bin/mainboard.cpp` 的调用链是：

```text
main
  -> ParseArgs
  -> BlockShutdownSignals
  -> ParseSchedulerFile
       -> google::protobuf::TextFormat::ParseFromString
       -> scheduler::SchedulerConf::FromProto
  -> SchedulerFactory::Create
  -> ModuleController::LoadAll
  -> WaitForShutdown(sigwait)
  -> ShutdownRuntime
```

Scheduler 必须在 DAG 加载前完成创建，因为之后
`Component::Initialize()` 会通过 `Scheduler::GetThis()` 注册协程任务。

精确路径：`bin/mainboard.cpp`、`src/scheduler/scheduler_factory.cpp`、
`src/scheduler/scheduler.cpp`、`include/minicyber/proto/scheduler_conf.proto`。

## 三、DAG、dlopen 与 Component 实例化

### 3.1 DAG TextFormat

`ModuleController::LoadAll()` 遍历 DAG 路径，调用
`LoadModuleFromFile()`。`ParseDagFile()` 读取文本后调用
`TextFormat::ParseFromString(content, DagConfig*)`。解析失败不会进入 DSO 边界。

精确路径：`src/mainboard/module_controller.cpp`、
`include/minicyber/proto/dag_conf.proto`、`config/autodrive/autodrive.dag`。

### 3.2 DSO 加载和注册

`ModuleController::LoadModule()` 对 `module_library` 执行：

```text
ResolveLibraryPath("libminicyber_autodrive_components.so")
  -> dlopen(path, RTLD_NOW | RTLD_GLOBAL)
  -> DSO 静态注册器构造
  -> ComponentFactory::Register(class_name, creator, owner)
  -> ComponentFactory::Create(class_name)
  -> shared_ptr<ComponentBase> 接管原始指针
  -> Component::Initialize(config)
  -> component_list_ 持有成功组件
```

`MINICYBER_REGISTER_COMPONENT` 分别出现在五个
`demo/autodrive/components/*_component.cpp`。`ComponentFactory::Instance()` 定义在
`src/component/component_factory.cpp`，不在 DSO 内形成第二个注册表。

`RTLD_NOW` 在 dlopen 时暴露未解析符号；`RTLD_GLOBAL` 使 DSO 中的工厂引用
解析到常驻 `minicyber_core`。这里是启动时动态加载，不是活跃组件在线替换。

### 3.3 Component 装配

单通道 `Component<M0>::Initialize()` 和双通道
`Component<M0, M1>::Initialize()` 均执行：

```text
Node(config.name)
  -> LoadConfigFiles
  -> 业务 Init
  -> Node::CreateReader（单输入一个，双输入两个）
  -> DataVisitor（单通道或 AllLatest 双通道）
  -> CreateRoutineFactory
  -> Scheduler::CreateTask
  -> ComponentBase::AttachRoutine
```

业务 `Init()` 使用 `GetProtoConfig()` 解析
`config/autodrive/components/*.conf`，并通过 Node 创建输出 Writer。

精确路径：`include/minicyber/component/component.h`、
`include/minicyber/component/component_base.h`、
`include/minicyber/component/component_factory.h`、
`demo/autodrive/components/`。

## 四、Node 端点与发现控制面

### 4.1 RoleAttributes 和 Join

`Node::CreateReader<T>()/CreateWriter<T>()` 委托
`NodeChannelImpl::CreateReader/CreateWriter`。`MakeRole<T>()` 填充：

- `host_name`、`host_ip=127.0.0.1`、`process_id`、`node_name`；
- `channel_name` 和 `Transport::ChannelNameToId()` 生成的 `channel_id`；
- Protobuf `message_type` 和序列化 `proto_desc`；
- 用于 Join/Leave 幂等和 opposite 集合的端点 `id`。

Reader/Writer 先用 `Transport::CreateHybridReceiver/Transmitter` 创建并 Enable Hybrid 端点，
再调用 `TopologyManager::Join(ROLE_READER/ROLE_WRITER, attr)`。如 Join 失败，刚创建的
Transport 后端会被 Disable，不会留下半初始化端点。

精确路径：`include/minicyber/node/node.h`、
`include/minicyber/node/node_channel_impl.h`、`include/minicyber/node/reader.h`、
`include/minicyber/node/writer.h`、`include/minicyber/transport/transport.h`。

### 4.2 FastRTPS Channel 发现

`TopologyManager::Join()` 调用链：

```text
TopologyManager::Impl::Join
  -> ControlParticipant::Start
  -> MakeChange(OPT_JOIN, ROLE_READER/WRITER, attr)
  -> Impl::OnChange（本地先应用）
       -> ChannelManager::Apply
       -> Impl::Notify(Hybrid change listeners)
  -> ControlParticipant::Publish（FastRTPS 控制面广播）
```

远端 Participant 收到 `ChangeMsg` 后也进入 `Impl::OnChange()`。
`ChannelManager` 用角色和 endpoint id 幂等更新快照。
`HasReader/HasWriter` 和 `GetReaders/GetWriters` 只读该快照。Participant 离开时，
`OnParticipantLeave()` 把该进程全部角色转成 Leave 通知。

精确路径：`src/topology/topology_manager.cpp`、
`src/service_discovery/channel_manager.cpp`、
`include/minicyber/proto/topology_change.proto`。其中 FastRTPS 的 `SerializedPayload_t`
只序列化拓扑 `ChangeMsg`，不序列化业务消息。

## 五、Hybrid 选路与扇出

`HybridTransmitter/HybridReceiver` 在构造时：

1. 创建 INTRA 和 SHM 后端；
2. 通过 `TopologyManager::AddChangeListener()` 订阅 Join/Leave；
3. 用 `ReconcileInitial()` 吸收创建前已存在的对端。

`OnTopologyChange() -> ApplyOpposite() -> RelationFor()` 将同一 host 内的对端分为：

- `SAME_PROC`：`intra_desired_=true`；
- `DIFF_PROC`：`shm_desired_=true`；
- `DIFF_HOST/NO_RELATION`：不启用数据后端。

`ReconcileBackends()` 在不持有 Hybrid 状态锁的区域调用后端 `Enable/Disable`，
避免 INTRA 回调内关闭 Writer 时重入自死锁。

`HybridTransmitter::Transmit()` 取当前已需要的后端快照，先 INTRA、后 SHM 分别发送。
`/autodrive/control_command` 同时有 mainboard 进程的 Audit Reader 和 Sink 进程的 Reader，
因此同一次 `ControlComponent::writer_->Write(command)` 同时触发两个后端。

精确路径：`include/minicyber/transport/transmitter/hybrid_transmitter.h`、
`include/minicyber/transport/receiver/hybrid_receiver.h`。

## 六、SHM Protobuf 字节链

### 6.1 写端

`ShmTransmitter<M>::Transmit()` 的调用链：

```text
M::SerializeToString(payload)
  -> PosixSegment::AcquireBlockToWrite(payload.size)
  -> memcpy(payload -> block.buf)
  -> block::set_msg_size
  -> PosixSegment::ReleaseWrittenBlock
  -> ConditionNotifier::Notify(ReadableInfo{block_index, channel_id})
```

Protobuf 序列化在申请写块前完成；普通 C++ 指针、`std::string` 对象布局或
`shared_ptr` 控制块都不会跨进程写入。

精确路径：`include/minicyber/transport/transmitter/shm_transmitter.h`、
`src/transport/shm/posix_segment.cpp`、`src/transport/shm/condition_notifier.cpp`。

### 6.2 读端

`ShmDispatcher` 的后台线程执行：

```text
ConditionNotifier::Listen
  -> ShmDispatcher::ReadMessage(channel_id, block_index)
  -> PosixSegment::AcquireBlockToRead
  -> 按 msg_size 拷贝为字节 string
  -> ShmDispatcher::NotifyRawListeners
  -> ShmReceiver<M> listener
  -> M::ParseFromString
  -> DataDispatcher<M>::DispatchFromShm
  -> Receiver::OnNewMessage -> Reader::Enqueue/用户回调
```

`DispatchFromShm/NotifyFromShm` 只唤醒标记 `receive_shm` 的 DataVisitor，防止跨进程副本
反向进入 INTRA Receiver 造成重复投递。Reader 的用户回调与 Component Proc 不是
同一边界：Source/Sink 可用 Reader 回调做进程端点，Component 只由协程链执行 Proc。

精确路径：`src/transport/dispatcher/shm_dispatcher.cpp`、
`include/minicyber/transport/receiver/shm_receiver.h`、
`include/minicyber/data/data_dispatcher.h`、`include/minicyber/data/data_notifier.h`。

## 七、INTRA 指针链

`IntraTransmitter<M>::Transmit(shared_ptr<M>)` 直接调用
`IntraDispatcher<M>::Dispatch(channel_id, msg)`，内部委托
`DataDispatcher<M>::Dispatch()`。Dispatcher 将同一 `shared_ptr` 填入该 Channel 的所有
ChannelBuffer，然后调用 `DataNotifier::Notify()`。

`IntraReceiver` 的 notifier 从自己的 ChannelBuffer 取最新指针并调用 Reader listener；
Component 的 DataVisitor notifier 调用 Scheduler 唤醒闭包。两者都不复制 Protobuf 对象。

Control 扇出中，`RuntimeEvidence::RecordControl()` 记录发布指针，
`ControlAuditComponent::Proc()` 内的 `RecordAudit()` 记录接收指针。两者相等是
同进程零拷贝证据；Audit 之后为外部集合对比主动创建副本，不改变
Control Writer 到 Audit Reader 的指针语义。

精确路径：`include/minicyber/transport/transmitter/intra_transmitter.h`、
`include/minicyber/transport/dispatcher/intra_dispatcher.h`、
`include/minicyber/transport/receiver/intra_receiver.h`、`demo/autodrive/evidence.cpp`。

## 八、DataDispatcher 到 DATA_WAIT/READY

### 8.1 Buffer 与 AllLatest

`DataVisitor<M0>` 构造时向 `DataDispatcher<M0>` 注册单 ChannelBuffer，并向
`DataNotifier` 注册同 channel notifier。`TryFetch()` 用绝对游标消费；游标落后
覆盖窗口时，`ChannelBuffer::Fetch()` 快进到最新元素。

`DataVisitor<M0, M1>` 为两个输入注册 Buffer，但 notifier 只注册到主通道 M0。
`AllLatest` 安装在主 Buffer 的 fusion callback：

```text
M1 Fill -> 只更新 secondary Buffer
M0 Fill -> primary fusion callback
        -> secondary.Latest()
        -> 写入独立 fusion Buffer
        -> DataNotifier::Notify(M0 channel)
```

Fusion 的 M0 是 `PerceptionObstacle`，M1 是 `VehicleState`。预热时 Source 先发
VehicleState sequence 0，再发 CameraFrame sequence 0，保证首个 PerceptionObstacle 驱动时
已有 VehicleState 最新值。

精确路径：`include/minicyber/data/data_visitor.h`、
`include/minicyber/data/fusion/all_latest.h`、`include/minicyber/data/channel_buffer.h`、
`include/minicyber/data/cache_buffer.h`。

### 8.2 唤醒回调的所有权

`Scheduler::CreateTask(RoutineFactory)` 从 factory 取得 DataVisitor，将 visitor notifier 绑定到
`Scheduler::NotifyTask(task_id)`。`RoutineWakeState` 处理“通知早于 task_id 发布”窗口：
先记 `notification_pending`，任务号可见后立即补一次 `NotifyTask()`。

`NotifyTask()` 取得 CRoutine，当状态是 `DATA_WAIT/IO_WAIT` 时调用
`SetUpdateFlag()`，然后只唤醒该协程原来所在的 Choreography Context 或 Classic group。
它不在通知线程直接把状态写为 READY，也不调用 Proc。

精确路径：`include/minicyber/data/data_visitor_base.h`、
`include/minicyber/data/data_notifier.h`、`src/data/data_notifier.cpp`、
`src/scheduler/scheduler.cpp`。

## 九、Scheduler、Processor 与上下文切换

### 9.1 任务创建和策略分叉

`Scheduler::CreateTask(func, name, prio, processor_id)` 构造 CRoutine，并按配置填充优先级、
group 或 processor id：

- Classic：`FindClassicGroup(name)` 将五个任务放入 `autodrive` 组，
  `ClassicContext::Enqueue()` 放入该组 20 级队列之一；
- Choreography：`FindChoreographyTask("perception")` 将 Perception 放入 processor 0 的
  `ChoreographyContext`；其余任务设 `processor_id=-1`，进入 `DEFAULT_GROUP_NAME`
  的 Classic 公共池。

`ClassicContext::NextRoutine()` 从高优先级到低优先级扫描组共享队列。
`ChoreographyContext::NextRoutine()` 只扫描本定向 multimap。两者都在
`CRoutine::Acquire()` 成功后调用 `UpdateState()`；更新标志使 `DATA_WAIT -> READY`。

精确路径：`src/scheduler/scheduler.cpp`、
`src/scheduler/policy/classic_context.cpp`、
`src/scheduler/policy/choreography_context.cpp`、
`config/autodrive/classic_sched.conf`、`config/autodrive/choreo_sched.conf`。

### 9.2 Resume 与 SwapContext

`Processor::Run()` 在工作线程中先调用 `CRoutine::GetThis()` 初始化线程主协程，
并设置本线程 Scheduler 指针。取得 READY CRoutine 后：

```text
Processor::Run
  -> CRoutine::Resume
  -> croutine::SwapContext(&thread_main_sp, &routine_sp)
  -> src/swap.S 保存调用者寄存并切换 rsp
  -> 首次进入 CRoutine::MainFunc，后续从 Yield 点继续
  -> RoutineFactory 循环 TryFetch
  -> Component::Process
  -> 业务 Component::Proc
  -> CRoutine::Yield(READY 或 DATA_WAIT)
  -> SwapContext(&routine_sp, &thread_main_sp)
  -> Processor::Run 归还 CRoutine::Release
```

`src/swap.S` 是 Linux x86_64 的实际上下文切换点；
`include/minicyber/croutine/detail/routine_context.h` 暴露 C++ 调用边界，
`src/croutine/detail/routine_context.cpp` 建立首次进入栈帧。

`CRoutine::MainFunc()` 在最终 Yield 前清空回调并释放协程栈上的临时
`shared_ptr`，再用裸指针切回主协程，避免永不再恢复的栈帧持有自身。

精确路径：`src/scheduler/processor.cpp`、`src/croutine.cpp`、
`include/minicyber/croutine/routine_factory.h`、
`include/minicyber/croutine/detail/routine_context.h`、`src/swap.S`。

## 十、五组件业务链

| 组件 | 输入 -> 输出 | 关键函数与路径 |
|---|---|---|
| Perception | CameraFrame -> PerceptionObstacle | `PerceptionComponent::Proc`，`demo/autodrive/components/perception_component.cpp` |
| Fusion | PerceptionObstacle + VehicleState -> FusedObstacle | `FusionComponent::Proc`，`demo/autodrive/components/fusion_component.cpp` |
| Planning | FusedObstacle -> Trajectory | `PlanningComponent::Proc`，`demo/autodrive/components/planning_component.cpp` |
| Control | Trajectory -> ControlCommand | `ControlComponent::Proc`，`demo/autodrive/components/control_component.cpp` |
| ControlAudit | ControlCommand -> ControlCommand 审计副本 | `ControlAuditComponent::Proc`，`demo/autodrive/components/control_audit_component.cpp` |

五个 Proc 只做确定性字段转换和简单数学计算，不引入 OpenCV/Eigen。
`source_sequence` 和 `source_monotonic_ns` 始终从 CameraFrame 链透传；Fusion 中的
VehicleState 只提供当次 AllLatest 数值，不改变链路身份。

`CreateAutodriveMessage()` 和 `CopyAutodriveMessage()` 的显式业务模板实例在
`demo/autodrive/autodrive_runtime.cpp` 的常驻 runtime 库中，避免核心库引入自动驾驶业务类型。

## 十一、sequence 0 端到端预热屏障

`demo/autodrive/sensor_source.cpp` 的实际预热顺序是：

1. 创建 `/autodrive/control_audit` Reader，回调只接受 sequence 0；
2. 创建 CameraFrame 和 VehicleState Writer，用 `HasReader()` 等待对端 Join；
3. 按 100 Hz 有界重发 VehicleState sequence 0；
4. 紧接着发布 CameraFrame sequence 0；
5. CameraFrame 依次经 Perception、Fusion、Planning、Control、ControlAudit；
6. ControlAudit 将 sequence 0 写入 `/autodrive/control_audit` SHM，Source 记录本地审计回执；
7. ControlSink 收到 `/autodrive/control_command` 的 sequence 0 后写跨进程预热文件；
8. Source 同时观察到两类回执后停止重发，再调用 `Rate::Sleep()` 建立测量节拍。

Sink 的 `Reader::HasWriter()` 与 Control Writer 的 `HasReader()` 是异步发现的两个方向，
不能用前者冒充双向收敛。双回执预热同时证明五组件、双通道融合、本地 INTRA Audit、
Control 跨进程 SHM 和返程 Audit SHM 已工作；sequence 0 不计入性能与完整性集合。

## 十二、结果统计与取证边界

`demo/autodrive/control_sink.cpp` 的两个 Reader 分别记录 Control 与 Audit 序列集合。
Control 回调用 `Metrics::RecordMeasurement()` 计算同一 `CLOCK_MONOTONIC` 时钟域下的
端到端延迟；Audit 回调记录独立集合。收齐后核对丢包、重复、乱序和
`audit_difference`。

`--metrics` 决定是否输出指标；`--evidence` 由脚本显式设置
`MINICYBER_AUTODRIVE_EVIDENCE_FILE`，用于记录组件线程和指针扇出。性能运行必须
关闭 evidence，不得把逐消息取证开销混入最终指标。

精确路径：`demo/autodrive/control_sink.cpp`、`demo/autodrive/metrics.cpp`、
`demo/autodrive/evidence.cpp`、`scripts/run_autodrive_pipeline.sh`。

## 十三、关闭、销毁与 dlclose

### 13.1 Component 与 DSO

mainboard 收到 SIGINT/SIGTERM 后，`ShutdownRuntime()` 首先调用
`ModuleController::Clear() -> RollbackTo(0, 0)`：

```text
逆序 ComponentBase::Shutdown
  -> is_shutdown_=true
  -> StopRoutine
       -> Scheduler::RemoveTask
       -> 撤销 RoutineWakeState
       -> CRoutine::Stop + Context::RemoveCRoutine
       -> data_visitor_.reset（RemoveNotifier 等待在途回调）
  -> Reader::Shutdown（Hybrid Disable -> Topology Leave）
  -> 业务 Clear（Flush evidence、释放 Writer）
  -> Node::Shutdown
清空 component_list_（在 DSO 仍加载时执行派生析构）
逆序 dlclose(lib_handles_)（静态注册器 Unregister）
```

`RemoveTask()` 先使 DataVisitor 唤醒状态不再持有 Scheduler，再停止和移除 CRoutine。
当 Component 在自身 Proc 内关闭时，Context 识别当前 CRoutine，不等待自己持有的
Acquire，避免自等待。

精确路径：`include/minicyber/component/component_base.h`、
`src/scheduler/scheduler.cpp`、`src/mainboard/module_controller.cpp`。

### 13.2 全局运行时

Component DSO 卸载后，mainboard 继续按顺序执行：

1. `ShmDispatcher::Shutdown()`：停 Listen 线程，等待在途 raw listener，释放 segment；
2. `TopologyManager::Shutdown()`：停 FastRTPS Participant，撤销变化监听并清 ChannelManager；
3. `Scheduler::Shutdown()`：先使 visitor 唤醒状态失效，再 Shutdown Context 唤醒 Wait，
   Stop/Join Processor，最后清任务映射与 Classic group。

Source/Sink 也先 `Node::Shutdown()`，再关 ShmDispatcher 和 TopologyManager。启动脚本在
正常路径最后执行 `control_sink --check-shm-clean`，只有失败或中断路径才用
`--cleanup-shm` 对四个精确 Channel 做环境恢复。

## 十四、调试时的快速定位点

| 现象 | 首个定位点 | 继续向下追踪 |
|---|---|---|
| DSO 没有组件 | `ModuleController::LoadModule` 的 dlerror | `ComponentFactory::Register/Create`、DSO 符号与注销 |
| Source 一直等 Reader | `Writer::HasReader` | `TopologyManager::Join/OnChange`、`ChannelManager::Apply` |
| SHM 收到但 Component 不运行 | `ShmReceiver::ParseFromString` | `DispatchFromShm`、`NotifyFromShm`、`Scheduler::NotifyTask` |
| 协程一直 DATA_WAIT | `RoutineWakeState` 的 task_id/pending | `SetUpdateFlag`、Context `UpdateState`、Processor `Resume` |
| Fusion 无输出 | `AllLatest` 的 secondary `Latest` | VehicleState 是否先到、primary fusion callback 是否执行 |
| Audit 有数据、Sink 无数据 | `ControlComponent` 的 SHM backend 快照 | opposite relation、`ShmTransmitter`、`ConditionNotifier` |
| 退出卡住或 dlclose 崩溃 | `ComponentBase::Shutdown` 顺序 | RemoveTask 在途回调、Reader Disable/Leave、对象销毁早于 dlclose |

更完整的排错命令、时序和历史 Bug 复盘见 `docs/DEBUG_MANUAL.md`；设计动机和
面试问答见 `docs/INTERVIEW_QA.md`。两份手册的最终扩写由 MC-622 完成。

## 十五、文件触达声明

最终分母取当前实际存在的 103 个留存生产/装配文件。证据来自清空旧 `.gcda` 后只运行一次
`test_autodrive_choreography_pipeline` 的 coverage 构建；它产生 38 个业务目标 `.gcda`，五个
组件翻译单元均有正执行行，核心 `scheduler.cpp`、`topology_manager.cpp`、Hybrid 收发模板等
均有正执行行。CMake 和 `.d` 仅用于确认目标归属，不单独判定触达。

下表每个单元格中的路径共享该行的“所属目标、业务入口、证据类型、结果”；路径仍逐一列出，
没有用目录名代替文件。纯声明/抽象头本身没有可执行行时，以已执行派生实现或所有者翻译单元
证明其职责被调用。`src/swap.S` 不支持 gcov，以 `ctx_swap` 符号、CRoutine 双向 ABI 测试和
业务组件实际运行在 Processor 线程三项交叉证明。

| 精确路径 | 所属目标 | 业务入口 | 运行时证据类型 | 结果 |
|---|---|---|---|---|
| `CMakeLists.txt`<br>`scripts/run_autodrive_pipeline.sh` | 构建与三进程启动 | CMake 生成；Classic/Choreography CTest 调用唯一脚本 | 目标清单 + 两套真实进程日志 | 通过 |
| `bin/mainboard.cpp` | `mainboard` | 参数解析 -> SchedulerFactory -> ModuleController | gcov 正执行 + 启动/关闭日志 | 通过 |
| `config/autodrive/autodrive.dag`<br>`config/autodrive/classic_sched.conf`<br>`config/autodrive/choreo_sched.conf` | mainboard 运行配置 | TextFormat 解析同一 DAG 与两种调度配置 | 两套 CTest 真实加载 | 通过 |
| `config/autodrive/components/control.conf`<br>`config/autodrive/components/control_audit.conf`<br>`config/autodrive/components/fusion.conf`<br>`config/autodrive/components/perception.conf`<br>`config/autodrive/components/planning.conf` | 组件 TextFormat 配置 | `ComponentBase::GetProtoConfig` | 五组件初始化日志 + 完整序列 | 通过 |
| `demo/autodrive/sensor_source.cpp`<br>`demo/autodrive/control_sink.cpp` | Source/Sink 可执行文件 | Rate 发布、双回执预热、Metrics 收口 | gcov 正执行 + 1000 条集合 | 通过 |
| `demo/autodrive/autodrive_runtime.cpp`<br>`include/minicyber/proto/autodrive_runtime.h` | `minicyber_autodrive_runtime` | 业务消息创建/复制模板实例 | gcov 正执行 + DSO 常驻符号 | 通过 |
| `demo/autodrive/evidence.cpp`<br>`demo/autodrive/evidence.h` | 功能取证 Runtime | 五组件、线程、指针、后端集合 | gcov 正执行 + evidence 文件 | 通过 |
| `demo/autodrive/metrics.cpp`<br>`demo/autodrive/metrics.h` | Sink 指标库 | `RecordMeasurement`、`RecordAudit` | gcov 正执行 + metrics 输出 | 通过 |
| `demo/autodrive/components/perception_component.cpp`<br>`demo/autodrive/components/fusion_component.cpp`<br>`demo/autodrive/components/planning_component.cpp`<br>`demo/autodrive/components/control_component.cpp`<br>`demo/autodrive/components/control_audit_component.cpp` | 唯一组件 DSO | dlopen -> 反射 -> 五段 Proc | 五个 `.cpp` gcov 均正执行 + 1..1000 集合 | 通过 |
| `include/minicyber/base/atomic_rw_lock.h`<br>`include/minicyber/base/macros.h`<br>`include/minicyber/base/rw_lock_guard.h`<br>`include/minicyber/common/types.h` | core 基础类型 | Choreography 队列锁、分支提示、NullType 特化 | 内联/模板 gcov 或已执行所有者 | 通过 |
| `include/minicyber/component/component.h`<br>`include/minicyber/component/component_base.h`<br>`include/minicyber/component/component_factory.h`<br>`src/component/component_factory.cpp` | core + 组件 DSO | 单/双输入装配、工厂注册、Shutdown | 模板与 `.cpp` gcov 正执行 + dlopen 日志 | 通过 |
| `include/minicyber/croutine/croutine.h`<br>`include/minicyber/croutine/detail/routine_context.h`<br>`include/minicyber/croutine/routine_factory.h`<br>`src/croutine.cpp`<br>`src/croutine/detail/routine_context.cpp`<br>`src/swap.S` | core 协程 | CreateTask -> Resume -> DATA_WAIT -> SwapContext | gcov 正执行；汇编符号 + ABI + 线程证据 | 通过 |
| `include/minicyber/data/cache_buffer.h`<br>`include/minicyber/data/channel_buffer.h`<br>`include/minicyber/data/data_dispatcher.h`<br>`include/minicyber/data/data_notifier.h`<br>`include/minicyber/data/data_visitor.h`<br>`include/minicyber/data/data_visitor_base.h`<br>`include/minicyber/data/fusion/all_latest.h`<br>`include/minicyber/data/fusion/data_fusion.h`<br>`src/data/data_notifier.cpp` | core 数据总线 | Dispatch -> Fill/Fusion -> Notify -> TryFetch | 模板/内联 gcov 正执行；抽象基类由 AllLatest 派生执行 | 通过 |
| `include/minicyber/mainboard/module_controller.h`<br>`src/mainboard/module_controller.cpp` | core mainboard | DAG -> dlopen -> Factory -> 回滚/卸载 | `.cpp` gcov 正执行 + 五组件加载/关闭日志 | 通过 |
| `include/minicyber/node/node.h`<br>`include/minicyber/node/node_channel_impl.h`<br>`include/minicyber/node/reader.h`<br>`include/minicyber/node/writer.h` | Source/Sink/组件端点 | CreateReader/Writer -> Join -> Hybrid -> Leave | 模板 gcov 正执行 + 双向发现/关闭证据 | 通过 |
| `include/minicyber/proto/autodrive.proto`<br>`include/minicyber/proto/component_conf.proto`<br>`include/minicyber/proto/dag_conf.proto`<br>`include/minicyber/proto/qos_profile.proto`<br>`include/minicyber/proto/role_attributes.proto`<br>`include/minicyber/proto/scheduler_conf.proto`<br>`include/minicyber/proto/topology_change.proto` | 业务/核心 Protobuf | 消息序列化、DAG/Scheduler 解析、发现 CDR | 生成代码 gcov + TextFormat/SHM/发现运行证据 | 通过 |
| `include/minicyber/scheduler/common/pin_thread.h`<br>`src/scheduler/common/pin_thread.cpp` | core 调度辅助 | Processor 创建时应用 affinity/policy | `.cpp` gcov 正执行 + Processor TID | 通过 |
| `include/minicyber/scheduler/policy/classic_context.h`<br>`src/scheduler/policy/classic_context.cpp` | core Classic | 共享 20 级队列、公共池竞争 | `.cpp` gcov 正执行 + Classic/公共池证据 | 通过 |
| `include/minicyber/scheduler/policy/choreography_context.h`<br>`src/scheduler/policy/choreography_context.cpp` | core Choreography | 定向 Processor 队列 | `.cpp` gcov 正执行 + 定向 TID 证据 | 通过 |
| `include/minicyber/scheduler/processor.h`<br>`include/minicyber/scheduler/processor_context.h`<br>`include/minicyber/scheduler/scheduler.h`<br>`include/minicyber/scheduler/scheduler_conf.h`<br>`include/minicyber/scheduler/scheduler_factory.h`<br>`src/scheduler/processor.cpp`<br>`src/scheduler/scheduler.cpp`<br>`src/scheduler/scheduler_factory.cpp` | core Scheduler | 配置转换 -> Processor -> Context -> Resume | `.cpp`/内联 gcov 正执行；抽象头由双策略执行 | 通过 |
| `include/minicyber/service_discovery/channel_manager.h`<br>`src/service_discovery/channel_manager.cpp`<br>`include/minicyber/topology/topology_manager.h`<br>`src/topology/topology_manager.cpp` | core 发现控制面 | Join/Leave -> FastRTPS -> 快照 -> Hybrid 变更 | `.cpp` gcov 正执行 + 晚加入/异常退出证据 | 通过 |
| `include/minicyber/time/duration.h`<br>`include/minicyber/time/rate.h`<br>`include/minicyber/time/time.h`<br>`src/time/duration.cpp`<br>`src/time/rate.cpp`<br>`src/time/time.cpp` | core 时间 | Source 节拍、轮询截止、单调时间戳 | 三个 `.cpp` gcov 正执行 | 通过 |
| `include/minicyber/transport/dispatcher/intra_dispatcher.h`<br>`include/minicyber/transport/dispatcher/shm_dispatcher.h`<br>`src/transport/dispatcher/shm_dispatcher.cpp` | core 数据分发 | INTRA 指针 Fill；SHM 通知解码分发 | 模板/`.cpp` gcov 正执行 + 双端集合 | 通过 |
| `include/minicyber/transport/receiver/receiver.h`<br>`include/minicyber/transport/receiver/intra_receiver.h`<br>`include/minicyber/transport/receiver/shm_receiver.h`<br>`include/minicyber/transport/receiver/hybrid_receiver.h` | core 接收端 | Reader -> HybridReceiver -> INTRA/SHM 回调 | 四个模板在主干 gcov 正执行 | 通过 |
| `include/minicyber/transport/shm/block.h`<br>`include/minicyber/transport/shm/condition_notifier.h`<br>`include/minicyber/transport/shm/posix_segment.h`<br>`include/minicyber/transport/shm/segment.h`<br>`include/minicyber/transport/shm/state.h`<br>`src/transport/shm/condition_notifier.cpp`<br>`src/transport/shm/posix_segment.cpp` | core POSIX SHM | AcquireBlock -> Serialize -> Notify -> Read | `.cpp`/内联 gcov 正执行 + 自然回收 | 通过 |
| `include/minicyber/transport/transmitter/transmitter.h`<br>`include/minicyber/transport/transmitter/intra_transmitter.h`<br>`include/minicyber/transport/transmitter/shm_transmitter.h`<br>`include/minicyber/transport/transmitter/hybrid_transmitter.h`<br>`include/minicyber/transport/transport.h` | core 发送端/工厂 | Writer -> Hybrid 按本地/远端关系并行扇出 | 五个模板 gcov 正执行 + 后端计数/指针证据 | 通过 |

逐项结果为 103/103 触达。唯一曾出现零执行行的生产翻译单元
`src/scheduler/processor_context.cpp` 只承载被双策略覆盖的默认三行实现，已将该原生默认职责
内联回 `processor_context.h` 并物理删除；没有为了提高覆盖率把接口改成偏离原生的纯虚函数。
