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
3. Sink 通过 `Reader::HasWriter()` 观察 Control Writer Join，写入 ready 文件；
4. 启动 `sensor_source --messages --frequency --ready-timeout-ms --await-shutdown`；
5. Source 通过两个 `Writer::HasReader()` 等待 Camera/VehicleState Reader Join；
6. Sink 收齐 Control/Audit 序列后退出，脚本用 SIGTERM 依次请求 Source 和 mainboard 关闭。

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
3. 发布 VehicleState sequence 0；
4. 发布 CameraFrame sequence 0；
5. CameraFrame 依次经 Perception、Fusion、Planning、Control、ControlAudit；
6. ControlAudit 将 sequence 0 写入 `/autodrive/control_audit` SHM；
7. Source 收到回执后 `WaitForWarmup()` 才返回，然后调用 `Rate::Sleep()` 建立测量节拍。

所以不得把预热描述成“只发 VehicleState，等一个 Rate 周期”。那样不能证明
五组件、双通道融合、Control 扇出和返程 SHM 均已就绪。

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

本调用链已按实际源码函数和主干运行证据复核。MC-619 coverage 目录的
38 个 `.gcda` 能证明对应翻译单元在 coverage 运行中被加载并产生运行数据；
但旧 `file_touch_report.txt` 中的 CMake、`.d` 或 include 标记只是构建可达证据。
本手册不使用这些标记宣称“100% 运行职责已触达”。

MC-623 需要对每个生产 `.cpp/.S` 给出 gcov 执行记录或明确的运行时
符号/日志证据，对模板和内联头给出业务实例化与执行证据；
不能证明职责的文件必须删除或明确处置，不得降低验收口径。
