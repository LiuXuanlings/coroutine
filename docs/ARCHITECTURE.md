# MiniCyber 架构说明

## 一、项目边界

MiniCyber 是面向 Linux x86_64、C++17 和 POSIX SHM 的 CyberRT 核心职责裁剪项目。
它保留三条可彼此贯通的主职责：

1. `mainboard + Component + DAG` 动态业务图装配；
2. `Node + Topology + HybridTransport + Data` 的 Channel 发布订阅；
3. `DataVisitor + Scheduler + CRoutine` 的数据驱动协程调度。

保留的生产模块为 `base`、`time`、`croutine`、`scheduler`、`data`、
`transport`、`service_discovery`的 Channel 最小子集、`topology`、`node`、
`component` 和 `mainboard`。公开 Channel API 只接受
`google::protobuf::Message` 派生类型。

本项目不声称完整复制 CyberRT。它的忠实性指“被保留的职责不改变
原生语义”，而不是“所有原生模块均存在”。

## 二、唯一业务主干

项目只保留一套自动驾驶业务管道：

```text
SensorSource 进程
  |  CameraFrame + VehicleState（SHM）
  v
mainboard 进程
  PerceptionComponent
  |  PerceptionObstacle（INTRA）
  v
  FusionComponent + AllLatest(VehicleState)
  |  FusedObstacle（INTRA）
  v
  PlanningComponent
  |  Trajectory（INTRA）
  v
  ControlComponent
  |-- ControlCommand（INTRA，同一 shared_ptr）--> ControlAuditComponent
  `-- ControlCommand（Protobuf -> SHM）-----------> ControlSink 进程
                                                         ^
  ControlAuditComponent -- ControlCommand（SHM）-----'
```

`SensorSource` 使用 `time::Rate` 产生输入节拍。sequence 0 不是简单的等待延时：
它的 `VehicleState` 与 `CameraFrame` 必须贯穿五个 Component，再由
`ControlAuditComponent` 通过 `/autodrive/control_audit` 回到 Source，才形成端到端
预热屏障。sequence 1 至 `--messages` 才进入功能和性能统计。

`config/autodrive/autodrive.dag` 是唯一业务 DAG。Classic 和 Choreography 不复制
业务图，只使用 `classic_sched.conf` 或 `choreo_sched.conf` 在调度器创建点分叉。

## 三、控制面与数据面

### 3.1 FastRTPS 只是发现控制面

`TopologyManager` 使用 FastRTPS Participant 发布和订阅 Channel `Join/Leave`
的 `ChangeMsg`。`ChannelManager` 保存 Reader/Writer 角色快照，并为
`HasReader()`、`HasWriter()` 和 Hybrid 路由提供事实。

FastRTPS 不承载 CameraFrame、ControlCommand 等业务数据。数据面只有：

- 同进程：INTRA，传递原 `shared_ptr`；
- 同机跨进程：POSIX SHM，传递 Protobuf 序列化字节。

跨主机对端不在项目支持范围。`DIFF_HOST` 不会被伪装为 SHM 或 RTPS
数据能力。

### 3.2 Hybrid 按对端关系扇出

`HybridTransmitter` 和 `HybridReceiver` 监听拓扑变化，按每个 opposite 的
`host_name/process_id/channel_name/id` 判定 `SAME_PROC` 或 `DIFF_PROC`。
一个 Writer 同时存在两类 Reader 时，一次 `Write()` 同时调用 INTRA 与 SHM
后端。`ControlComponent` 的 `/autodrive/control_command` 正是该语义的主干证据：
Audit 观察同一指针，Sink 观察反序列化副本。

## 四、数据与协程边界

`DataDispatcher<T>` 对 ChannelBuffer 弱所有，发布时先取快照、后填充。
`DataNotifier` 对唤醒回调取快照，注销时等待在途调用。这两层不执行
Component 业务逻辑。

`DataVisitor<M0>` 持有单 ChannelBuffer；`DataVisitor<M0, M1>` 只在主通道
M0 到达时，通过 `AllLatest` 取 M1 最新值。该语义不是时间戳对齐、
不是 Barrier，也不会因 M1 单独到达触发 `FusionComponent::Proc()`。

`RoutineFactory` 在每次尝试取数据前将协程标记为 `DATA_WAIT`。数据到达后，
`DataNotifier -> Scheduler::NotifyTask()` 只设置更新标志并唤醒 Processor；
Processor 在 Context 中调用 `UpdateState()` 才把它转为 `READY`，然后
`CRoutine::Resume()` 恢复协程栈并进入 `Component::Proc()`。因此发布线程
不同步执行 Component 业务回调。

## 五、调度策略是唯一主干的分叉

### 5.1 Classic

Classic 的正式表述是“调度组共享多级优先级队列”。一个 group 持有
20 级共享队列，组内多个 Processor 从高优先级向低优先级竞争就绪协程。
`CRoutine::Acquire()` 只防止同一协程被并发 `Resume()`，不是工作窃取。

### 5.2 Choreography

Choreography 保留原生双区：配置 `processor` 的 `perception` 任务进入定向
`ChoreographyContext`；其余四个任务未配置 Processor，进入 Classic 公共池。
定向区不窃取公共池任务，公共池也不进入定向 Context。

## 六、主要模块职责映射

| MiniCyber 边界 | 保留职责 | `cyber_ref` 对照 |
|---|---|---|
| `mainboard/ModuleController` | DAG TextFormat、动态库加载、组件创建和回滚 | `cyber/mainboard/module_controller.cc` |
| `component/Component` | Node/Reader/DataVisitor/RoutineFactory 装配、`Init/Proc/Clear` | `cyber/component/component.h` |
| `node` | RoleAttributes、Reader/Writer 工厂与端点生命周期 | `cyber/node` |
| `topology + ChannelManager` | Channel Join/Leave、对端快照和变化监听 | `cyber/service_discovery/topology_manager.cc` |
| `HybridTransport` | 按 opposite relation 开关并扇出后端 | `cyber/transport/transmitter/hybrid_transmitter.h` |
| `DataVisitor + AllLatest` | ChannelBuffer 消费游标、主通道驱动融合 | `cyber/data/data_visitor.h` |
| `RoutineFactory` | `DATA_WAIT`、TryFetch 和 Proc 循环 | `cyber/croutine/routine_factory.h` |
| `Scheduler + Context + Processor` | 任务创建、数据唤醒、策略路由和执行 | `cyber/scheduler/scheduler.cc` |
| `CRoutine + swap.S` | 状态机、用户栈和 x86_64 上下文切换 | `cyber/croutine` |

MiniCyber 以 `ComponentFactory` 替代完整 Poco ClassLoader 体系，但仍强制
`.so + dlopen(RTLD_NOW | RTLD_GLOBAL) + 静态注册`。这是对工程范围的裁剪，
不是把静态链接伪装成原生动态加载。

## 七、明确删除的能力

| 删除项 | 原因与边界 |
|---|---|
| Timer/TimerComponent/TimingWheel | 时间轮需要独立复杂生命周期；数据源用独立进程和 `time::Rate` 表达节拍。 |
| RPC service/client | 唯一主干只需 Channel 发布订阅；保留 RPC 会引入第二套业务模型。 |
| RTPS 数据面 | 本项目的数据面固定为 INTRA/SHM；FastRTPS 只保留 Channel 发现控制面。 |
| 三、四输入 Component/DataVisitor | 唯一主干只实例化单输入与双输入 AllLatest，不保留无业务证据的模板面。 |
| 旧 Demo、benchmark 和历史性能数据 | 防止离散路径给出与主干不同的语义和性能口径。 |
| 跨主机 Channel | 平台边界是同机 POSIX SHM，不宣称网络数据能力。 |

## 八、生命周期与所有权

- 启动时先创建 Scheduler，再 dlopen Component DSO；否则 Component 无法注册协程。
- Reader/Writer 先创建 Hybrid 端点再 Join，失败时停止已创建后端。
- Component 关闭先阻止 Proc，再 RemoveTask、撤销 DataVisitor、关 Reader，最后 Clear Writer 和 Node。
- ModuleController 按逆序 Shutdown 组件，销毁组件后才能 `dlclose`。
- mainboard 最后关闭 ShmDispatcher、TopologyManager 和 Scheduler，避免 DSO 卸载后再有回调进入。
- POSIX SHM 按块锁和段所有权回收；正常验收不用脚本强制 unlink 伪造自然回收。

## 九、证据声明

MC-619 的 `file_touch_report.txt` 已冻结生产文件分母，coverage 构建目录
中存在 38 个 `.gcda`，并且主干证据确认五组件、动态加载、
Choreography 双区和 Control 混合扇出。但该报告对多数文件的标记是
`explicit_link_or_compile_chain` 或 `production_compile_dependency`，这只证明构建可达，
不能单独证明每个运行职责已执行。

因此本文档只声明已核对的架构和主干路径，不声称“100% 生产文件运行职责
已触达”。最终逐文件 gcov/运行时符号/日志证据和未触达文件处置由
MC-623 收口。MC-620 数据因取证开关开启且工作区为 dirty，仅作临时观测，
不在本文档写成最终性能结论。
