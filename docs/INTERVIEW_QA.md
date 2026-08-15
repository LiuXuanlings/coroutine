# MiniCyber 面试问答

> 本文由 MC-602 迁移历史知识，并由 MC-622 依据第二次重构的源码、集成测试和真实排错
> 证据定稿。回答必须区分已验证能力、项目边界和 MC-623 尚待完成的最终验收，不把计划
> 或临时性能样本包装成结论。

建议按四层递进口述：先用一句话回答“为什么”，再说明 CyberRT 原生职责和 MiniCyber 的
保留/删除边界，然后给出源码调用链或测试证据，最后主动说明平台、可靠性和性能限制。
面试时可依次使用“项目定位 -> 协程与调度 -> 数据与传输 -> Component/dlopen -> 完整主干
-> 测试性能 -> 不具备能力”，避免堆砌孤立类名。

## 项目定位

### MiniCyber 要解决什么问题？

MiniCyber 是 Linux x86_64、C++17 和 POSIX SHM 上的 CyberRT 精简实践。最终用一条
自动驾驶消息链验证栈式协程、两种 Scheduler、Node/Reader/Writer、INTRA、Protobuf
SHM、发现控制面、动态组件和优雅退出如何协作，而不是实现完整 Apollo 或通用自动驾驶
平台。

### 业务消息为什么统一携带源序列和单调时间？

MC-614 在 CameraFrame、VehicleState、PerceptionObstacle、FusedObstacle、Trajectory 和
ControlCommand 的字段 1、2 固定为 `source_sequence` 和
`source_monotonic_ns`。这沿用 CyberRT 示例 Header 将序列与时间作为消息关联键的职责，
但不增加可被组件遗漏的嵌套 Header。传感器为测量样本生成从 1 开始的连续序列和
`CLOCK_MONOTONIC` 时间；各组件只透传它们，ControlSink 才能用同一时钟域计算完整链路
延迟、按期望集合判定丢包并识别重复。序列 0 供 VehicleState 与 CameraFrame 端到端预热使用，不进入测量
集合。这不是严格时间同步或可靠传输承诺，AllLatest 仍按主通道到达时取次通道最新值。

### 为什么只保留一条业务主干？

多套 talker、listener、ping-pong 和独立 benchmark 容易绕过真实生命周期。唯一主干
要求每个留存文件进入同一静态调用链或运行时加载链，使功能、资源检查和性能使用同一
口径；它突出中间件职责，并不试图展示复杂算法。

## 协程和调度

### 为什么保留栈式 CRoutine，而不改用 C++20 coroutine？

范围对齐 CyberRT 的 x86_64 `SwapContext`、协程状态和数据驱动调度。标准 C++20
coroutine 只定义帧和挂起语义，不提供执行器、跨线程唤醒、背压或跨进程传输；另建
运行时会测量另一套队列和唤醒策略。因此旧 C++20 编译/恢复探针已删除，不作为性能
或架构对照。

### Classic 和 Choreography 分别解决什么问题？

Classic 的定稿语义是一个调度组共享 20 级优先级队列，同组 Processor 从高到低竞争
任务；`Acquire` 只避免同一协程被并发执行。它不是工作窃取，也不为每个 Processor
建立 `proc_i` 私有组。

Choreography 保留双区：显式 `processor_id` 的任务进入定向 Processor，未指定任务
进入 Classic 公共池。同一 DAG 只替换 Scheduler 配置，不复制业务链。共享队列和公共池
已分别由 MC-610、MC-611 验证，并在 MC-618/MC-619 的同一业务主干中验收。

### 为什么 Reader 回调不能直接执行业务 Proc？

发布线程只负责接收和填充。正确链路是 `DataNotifier -> Scheduler::NotifyTask ->`
`DATA_WAIT/READY -> Processor -> CRoutine::Resume -> Proc`。这样业务执行归属
Processor，关闭时能切断任务和 Reader，避免发布线程进入已卸载组件。

## 数据和传输

### AllLatest 是严格时间同步吗？

不是。第一通道是主驱动，每次主消息到达组合当前主消息和第二通道最新值；次通道无值
就不产生融合结果。填充 notifier 必须早于 Scheduler 唤醒 notifier，且融合游标只能
推进一次。业务启动先预热 VehicleState，避免第一条主输入缺少次输入。

### 为什么 DataVisitor 不再提供直接阻塞 Fetch？

DataVisitor 的职责是维护 ChannelBuffer、消费游标和 DataNotifier，而不是在任意调用线程
阻塞或执行 Component 业务。MC-609 只保留 `TryFetch`，由 RoutineFactory 创建 CRoutine
循环：每轮先标记 `DATA_WAIT`，有数据时才调用 Proc 并 `Yield(READY)`，无数据则保持等待。
调度层通过 `DataVisitorBase` 注册 `NotifyTask` 回调，把数据到达转换为协程唤醒；发布线程
只负责 Dispatch。这样关闭时可以先解除 notifier，再回收协程和 Reader，不会把业务执行
留在 Transport 回调栈上。

### 为什么 RoutineFactory 只支持单/双输入？

唯一业务链只有单通道处理和 Fusion 的双通道 AllLatest。双通道由首通道驱动：次通道更新
最新值但不唤醒协程，首通道 Fill 在 DataNotifier 唤醒前生成融合对；访问器的游标对每个
成功融合恰好前进一次。三/四输入模板会扩大未验证的 API 与关闭组合，MC-609 已物理删除，
不以泛型外观伪装项目未支持的多通道同步能力。

### INTRA 和 SHM 的零拷贝边界是什么？

INTRA 只在同进程传递同一个 `shared_ptr`，可证明对象身份不变。跨进程不能共享带有
动态内存和虚表语义的 C++ 对象，SHM 必须写入 Protobuf 序列化字节并在接收端构造新
消息。对外 Channel API 因此只接收 `google::protobuf::Message` 派生类型。

### 为什么需要 HybridTransport？

同一个 Writer 可能同时面对本进程 Audit Reader 和外部 Sink Reader。最终
HybridTransport 按每个对端并行维护连接：同进程走 INTRA，其他同机进程走 SHM；它
不是创建端的一次性全局二选一，也不允许重复投递。MC-607 已由
`HybridTransmitter`/`HybridReceiver` 订阅 ChannelManager 的角色变化：同一类对端
只启用一个后端，最后一个对端 Leave 才停用该后端；MC-608 再把该能力接入
Protobuf-only Node API 和端点 Join/Leave 生命周期。

### 如何在完整业务链中证明同一个 Writer 的混合扇出？

MC-619 只使用唯一 `autodrive.dag`，让 `ControlComponent` 的
`/autodrive/control_command` Writer 同时面对本进程 `ControlAuditComponent` 与独立
`ControlSink`。验收快照只读取 Hybrid 已应用的 INTRA/SHM 后端状态，不向业务暴露选择后端的
新 API；每个测量序列记录 Control 发布对象地址和 Audit 接收对象地址，二者 1000 次均相同，
证明 INTRA 传递同一 `shared_ptr`。Sink 记录不同 PID 中反序列化后的对象地址，并分别导出
Control/Audit 的 `source_sequence` 集合；两集合均严格为 1 至 1000，重复和差异均为 0。
Audit 的二次发布只用于让 Sink 比较结果，不能冒充 Control Writer 的 SHM 证据。

同一运行还用 Choreography 配置把 Perception 送入定向 Processor，其余四个 Component 回退
Classic 公共池。该证据验证原生双区职责及按对端扇出，不是性能结论；吞吐和延迟比较仍属于
MC-620 的 Release 采集。

### HybridTransport 如何保证跨进程数据不退化为普通对象 memcpy？

Hybrid 的公开模板以 `static_assert` 限定 `google::protobuf::Message` 派生类型。
INTRA 直接把原 `shared_ptr` 分发给本进程 Reader；SHM 只写 `SerializeToString` 的字节，
接收端用 `ParseFromString` 构造新的消息对象。`ShmDispatcher` 在释放 SHM 块前复制本次
字节并对注册监听器维护在途计数，注销后不会再回调已关闭的 Receiver。字符串收发器仅为
MC-603 冻结的底层 SHM 回归保留兼容特化，不属于 Hybrid 或后续 Channel 正式 API。

验证是 `test_hybrid_transport`：纯 INTRA 断言指针身份不变；纯 SHM 断言含 `bytes`
动态字段的 Protobuf 恢复；混合角色断言同一 Writer 向本地和跨进程 Reader 各投递一次，
并在远端 Leave 后停止 SHM 扇出。项目仍不包含 RTPS 数据面或跨主机数据连接。

### 为什么 Hybrid 不能持状态锁调用 INTRA Transmit 或后端 Disable？

INTRA 是同步路径，`Transmit` 可能在当前发布线程直接进入用户回调；Receiver 的 Disable
还可能等待已经开始的回调结束。如果 Hybrid 持有自己的状态锁调用这些路径，回调内关闭
Writer、Reader 或 Node 会回入同一对象，形成自死锁或跨线程等待环。当前实现只在锁内维护
对端集合和后端期望状态，实际分发、Enable、Disable 均在锁外执行；原子协调标志保证并发
拓扑变化最终收敛。这里保留的是 CyberRT 生命周期分层，不是新增调度优化。

### 为什么正式 Protobuf SHM 两端必须使用一致段布局？

POSIX SHM 的块大小由第一个创建同名段的进程写入共享 State。若 Reader 默认创建 1 KiB，
Writer 却按 64 KiB 假设发送，同一 Channel 的能力就会取决于启动顺序。原生 CyberRT 用
`ShmConf` 按消息大小分档并支持容量不足时重建；MiniCyber 为控制范围，不恢复完整动态
分档，但正式 Protobuf Reader/Writer 必须共同采用原生首档的 16 KiB/512 块，并在既存
布局更小时明确初始化失败。512 个未读块避免 Dispatcher 短时调度延迟覆盖尚未消费的
通知；项目仍不宣传动态扩容能力。

### Node 为什么要为每个端点填充完整的 RoleAttributes？

原生 CyberRT 的 `NodeChannelImpl::FillInAttr` 在创建 Reader/Writer 时填入主机、进程、
节点、Channel、消息类型和 Protobuf 描述符等元数据。MC-608 保留这一职责：MiniCyber
从同一份 `RoleAttributes` 生成 Channel ID、端点 ID 和消息描述符字节，并将它同时交给
ChannelManager 与 HybridTransport。这样发现状态、连接匹配和消息类型没有第二套来源；本机
范围内固定 `host_ip=127.0.0.1`，不因此声称支持跨主机数据面。

### Writer::HasReader 为什么不能用固定 sleep 代替？

`HasReader` 查询 ChannelManager 的当前对端状态，是 Writer 在发送前可观察的就绪门禁；它
不承诺消息已经被消费，也不替代可靠数据确认。固定 sleep 既无法覆盖晚 Join，也会掩盖
Leave 后的无对端状态。`test_node` 以动态 Join/Leave 验证 Writer 与 Reader 的双向可见性，
并在 Reader 离场后确认 Writer 不再向已关闭回调投递。

### Node 端点为什么按 Disable 再 Leave 关闭？

端点先禁用 Hybrid 的 INTRA/SHM 后端，再从 ChannelManager Leave。这个顺序对齐 CyberRT
端点撤销连接的生命周期意图：Leave 触发的拓扑变更不能再使正在关闭的端点恢复任何传输
连接；随后析构 Hybrid 时移除 ChangeListener。关闭和重复调用均为幂等，Node 仍可统一关闭
调用方继续持有的 Reader/Writer。Reader 回调只维护有界 Observe 队列，业务 Proc 的协程
调度仍属于 MC-609/MC-612。

### FastRTPS 在项目中承担什么角色？

FastRTPS 只负责同机 Channel Join/Leave 控制面。数据面只有 INTRA 和 POSIX SHM，
不构建 RTPS 数据收发类型，也不声称支持跨主机数据。系统包通过
`find_package(FastRTPS REQUIRED)` 提供；MC-604 已验证当前系统导出 `fastrtps` target。
MC-606 以私有 CDR 字节序列承载 Protobuf `ChangeMsg`，使两个同机进程能传播
Writer/Reader Join 和 Leave；Participant 离场监听会清除该进程残留角色。CDR 封装只服务
控制 Topic，不改变数据面边界。

### 为什么不能把所有拓扑状态继续放在 TopologyManager？

原生 CyberRT 的 TopologyManager 负责总编排，具体 Channel 角色由 ChannelManager 管理。
MiniCyber 即使裁剪 NodeManager 和 ServiceManager，也应保留这一职责边界。当前
ChannelManager 是 Writer/Reader/Node 状态唯一所有者，负责幂等应用 Join/Leave、查询快照
和按进程清理；TopologyManager 只负责 Participant、控制消息校验和 ChangeListener 生命周期。
这样 MC-607/608 只消费一个发现快照，不会出现本地表与控制面表互相矛盾。

### 为什么拓扑 Topic 使用 Reliable、Transient Local 和 Keep All？

这组配置对齐原生 `QOS_PROFILE_TOPO_CHANGE`。Reliable 降低控制变更丢失风险，Transient
Local 让晚加入订阅者获得仍由 Publisher 保存的历史，Keep All(depth=10) 避免紧邻的 Join
和 Leave 被压缩成一个最终样本。它服务的是低频控制面，不代表业务数据都应使用相同 QoS，
也不意味着项目开放 RTPS 数据传输。

### 为什么过滤本进程 FastRTPS 回环消息？

本地 Join/Leave 需要立即写入 ChannelManager 并通知监听器，同时还要广播给其他进程。
若本进程 Subscriber 再消费自己的控制样本，同一个操作会通知两次。以 host 和 PID 过滤
自回环，使本地一次操作精确产生一次事件；远端进程仍正常接收广播。ChannelManager 的
幂等 Apply 是第二层防御，但不能用“最终状态没重复”掩盖监听器重复执行。

### 为什么 Protobuf 字段和枚举删除后不能重新连续编号？

字段编号和枚举值是序列化协议契约，不是源码显示顺序。MiniCyber 裁剪原生字段时仍要保留
原编号，例如 `CHANGE_CHANNEL=2`、`ROLE_WRITER=2`、`ROLE_READER=3`，SchedulerConf 的
`routine_num` 从 2 开始。重排后同名消息在二进制层会被解释为其他字段，破坏与原生配置和
历史数据的兼容。删除项应留空或 `reserved`，不能为了美观压缩编号。

### MC-604 至 MC-606 分别建立了哪些可验证边界？

MC-604 把递归 GLOB 改为显式源清单，建立系统 FastRTPS 必需依赖和 Scheduler Protobuf
骨架；MC-605 将 Time、Duration、Rate 收拢到 `minicyber::time`，SensorSource 可用 Rate
表达输入节拍而不恢复 Timer；MC-606 建立同机 Channel 控制面、ChannelManager 状态层和
双向跨进程 Join/Leave 证据；MC-607 建立 Protobuf Hybrid 扇出；MC-608 将完整
RoleAttributes、Protobuf-only Node API、动态 `HasReader` 和安全端点关闭接入该路径。完整
业务链仍属于 MC-609 以后，不能把这些计划能力提前说成已实现。

## 插件、范围和可靠性

### 为什么强制业务组件使用 `.so + dlopen`？

这保留 mainboard 的真实插件边界。`dlopen(RTLD_NOW | RTLD_GLOBAL)` 触发共享库静态
注册，主进程和插件必须解析到唯一 ComponentFactory；随后由工厂创建组件。失败回滚
按本次加载水位逆序 Shutdown、销毁组件和 `dlclose`，不把旧测试的静态预注册绕过
当作生产能力。

### 为什么业务 `.so` 不能再次链接静态 Protobuf 生成库？

Protobuf 生成文件会在库加载时向进程级描述符数据库注册。MC-615 的主程序已经经
`minicyber_core` 持有 `minicyber_proto`；若业务 `.so` 又直接链接该静态库，`dlopen` 会让
同一 `.proto` 第二次注册并在 `GeneratedDatabase()->Add` 失败。业务插件因此只链接核心库，
从核心库继承生成头文件和运行时符号。这个约束与 ComponentFactory 的唯一实例相同，都是
跨 DSO 不能分裂的 ABI 前提；它不表示支持任意第三方 Protobuf ABI 混用。

### 为什么可卸载组件要把数据总线和消息控制块放入核心库？

CyberRT 的 ClassLoader 边界要求先销毁组件、再卸载库，工厂也必须同时撤销该库的创建函数。
MiniCyber 的业务组件是同一可卸载 `.so`，因此 `DataDispatcher`、`DataNotifier` 和消息
`shared_ptr` 控制块不能在头文件模板或插件中各自生成。否则一方面两份数据总线互不通信，
另一方面 GNU unique 符号会使 glibc 把 DSO 标记为 `NODELETE`，或在卸载后留下指向插件代码的
析构入口。MC-615 将业务 Dispatcher 显式实例化、Notifier 单例和输出消息工厂固定到
`minicyber_core`，插件仅引用；测试还在 `dlclose` 前释放观察副本。该方案保证本项目这一条
业务链的卸载安全，不承诺任意插件对象或用户长期持有消息时都可热卸载。

### 为什么删除 Timer、TimerComponent 和 RPC？

它们不服务唯一主干，并会引入额外线程、生命周期或另一套通信边界。输入节拍由独立
SensorSource 的 `time::Rate` 控制，不把 Rate 包装成伪 Timer；RPC 不用 Channel
重新包装。删除是范围裁剪，不是宣称这些能力没有价值。

### 如何处理 SHM 的异常退出？

命名 SHM 不会随进程退出自动 unlink。创建者正常销毁时负责解除映射并删除名称；异常
信号路径只能读固定登记表并调用异步信号安全的 `shm_unlink`，再恢复默认处理。不能
在信号处理器里加锁、分配内存或调用普通析构逻辑。

## 测试和性能

### 为什么不追求大量单元测试？

测试按风险保留：x86_64 ABI、CRoutine 生命周期、Notifier 注销、AllLatest、SHM 信号
清理、发现、Hybrid、两种 Scheduler、Component 线程归属和 dlopen 回滚。完整自动驾驶
流水线才是主验收，因为它同时证明组件、协程、传输和进程退出没有被测试替身绕开。

### 性能结果应该怎样解释？

只测 Release 下的完整自动驾驶主干，指标为端到端 p50/p95/p99、吞吐、丢包和审计
一致性。Classic 与 Choreography 使用相同 DAG 和输入；结果记录环境、依赖、提交和
工作区状态。MC-620 已形成方法和临时观测，但样本采集时存在 dirty 工作区及功能 evidence
开销，不能作为最终数字；MC-623 必须在已提交、干净且关闭 evidence 的 Release 基线上重采。
不得将已删除的 PIPE、互斥队列、ping-pong 或 C++20 探针数值拿来比较，也不得从单次测量
推出普适优势或两种策略的普遍高下。

### 为什么性能原始结果同时保存 CSV 和 JSON？

MC-620 定义的 CSV 固定提供提交、策略、消息数、频率、样本数、p50/p95/p99、吞吐、丢包、重复和
单调时钟运行时长，便于按统一字段比较。JSON 保留同一指标外的工作区状态、CPU、内核、编译器、
FastRTPS/Protobuf 版本、系统负载、DAG/Scheduler 哈希、启动参数和 SHM 自然回收结果，避免
脱离环境解释数字。采集脚本只调用唯一 `run_autodrive_pipeline.sh`，并在每种策略前后检查四个
精确 SHM 名称；它不建立基准替身，也不把收集器变成业务数据路径的一部分。当前文件只说明
口径，不引用 MC-620 临时数字冒充最终性能结论。

## 项目不具备的能力

MiniCyber 不支持 Windows、aarch64、跨主机 RTPS 数据、RPC、TimerComponent、录包
回放、参数服务、监控、Python 接口或旧离散 Demo。它也不承诺可靠持久队列、严格时间同步、
组件热重载或通用第三方 Protobuf ABI。当前完整主干与 FastRTPS 系统依赖已经验证，但最终
逐文件职责验收、sanitizer 收口和干净 Release 性能重采仍属于 MC-623。

## 证据来源

本文迁移自首轮模块映射、调度/传输/组件笔记与 00 历史记录，并由 MC-622 对照
`docs/ARCHITECTURE.md`、`docs/CALL_CHAIN.md`、现存源码和测试证据定稿。架构决策以
`docs/refactor/02_架构取舍矩阵.md`、验收定义以
`docs/refactor/03_验收与性能口径.md` 为准；性能最终数字以 MC-623 干净 Release 重采为准。

## Classic 调度组

### Classic 为什么必须由调度组共享 20 级队列？

原生 `SchedulerClassic` 为每个 `SchedGroup` 创建多个 Processor，并让它们的
`ClassicContext` 指向同一组多级就绪队列。MC-610 保留该职责：`SchedGroup` 的
`processor_num` 决定共享消费者数量，具名 `ClassicTask` 决定协程所在组和优先级，
未知任务进入第一个默认组。每个 Processor 不再拥有 `proc_i` 私有组；高优先级由各
Processor 从第 19 级到第 0 级统一扫描决定。

`Acquire` 不负责跨组分发或负载策略，它只防止同一个 CRoutine 被两个同组 Processor
同时 Resume。数据到达时，`RoutineFactory` 交出的 `DataVisitorBase` 只注册
`Scheduler::NotifyTask`，把 `DATA_WAIT` 标记为可更新并唤醒所属组，发布线程不会直接
调用 Proc。关闭 Scheduler 时先让 Visitor 回调失去 Scheduler 指针，再停止 Processor
并删除静态组状态，避免生命周期更长的 Visitor 回调已销毁调度器。

验证由 `test_scheduler_classic` 的 Protobuf 配置映射和共享组任务、
`test_processor` 的完整候选集优先级顺序，以及 `test_routine_factory` 的通知唤醒覆盖；
三者均已纳入 `high_risk`，并与 `test_classic_context` 连续运行 20 轮通过。

### Choreography 为什么要同时保留定向区和 Classic 公共池？

原生 Choreography 为配置了 `processor_id` 的任务建立定向执行上下文，同时保留未绑定
任务可进入的调度组。MC-611 按 `choreography_processor_num` 创建定向 Processor，按
`pool_processor_num` 创建 Classic 公共池；显式且有效的 Processor 编号只进入定向区，
未配置或越界编号统一进入公共池的共享多级队列。这样保留目标处理器亲和职责，又复用
MC-610 的 Classic 队列语义，没有复制第二套优先级队列或把错误配置轮询到定向区。

数据到达仍通过 `Scheduler::NotifyTask` 回到任务创建时的原路由：定向任务唤醒其
`ChoreographyContext`，公共池任务唤醒 Classic 公共组。`test_scheduler_choreography`
覆盖线程归属、越界回退和两条 DATA_WAIT 唤醒路径，并与 Classic 高风险测试连续 20 轮
通过。SchedulerFactory 只接受 `classic` 和 `choreography`，未知策略直接拒绝，避免配置
错误伪装成另一种调度语义。

### 为什么修改 SchedulerConf 后必须完整重编译？

`SchedulerConf` 是 Scheduler 与测试/调用方共享的公开 C++ 结构。MC-611 增加 Choreography
双区字段会改变对象布局；若只重编译部分目标，旧对象按原布局访问新栈对象，可能表现为
`stack smashing`，这不是可接受的 ABI 兼容策略。一次旧对象失败后，完整 Debug 构建和
`test_routine_factory` 连续 20 轮通过，证明问题归属于构建收口而非运行时调度逻辑。

## Component 协程执行

### Component 为什么不能在 Reader 回调中直接调用 Proc？

原生 CyberRT 的 reality mode 由 `DataVisitor` 订阅 Channel、`RoutineFactory` 创建数据
协程、Scheduler 注册 notifier 唤醒；Reader 端点本身不拥有业务执行线程。MC-612 保留这条
职责链：单/双输入 Component 先完成 `Init` 和无同步 callback 的 Reader 创建，再按 Reader
的 Channel id 创建 DataVisitor 和 RoutineFactory，最后向 Scheduler 提交任务。发布线程只
执行 `Dispatch -> DataNotifier -> Scheduler::NotifyTask`，Proc 只在 Processor 的
`CRoutine::Resume` 中运行。

双输入的首 Reader 是 AllLatest 主驱动，次输入只填充最新值；没有次输入时首输入不会融合，
不把它伪装为时间戳严格对齐。`test_component` 同时验证 Proc 线程不同于发布线程、首输入
驱动，以及 Component 自身 Proc 中关闭可有界返回。

### Component 如何关闭，为什么这个顺序不能交换？

MC-612 的 ComponentBase 固定为“禁止新 Proc -> RemoveTask 解除 Visitor 唤醒 -> 注销
Reader -> Clear 释放业务 Writer/资源 -> 关闭 Node”。先注销 Reader 会留下已注册的
DataVisitor notifier，发布线程可能仍唤醒已拆除的任务；先释放 Writer 又可能在业务资源
失效后继续运行 Proc。Scheduler 移除当前 Proc 时识别当前 CRoutine，不等待它自己的
`Acquire`，避免 `Proc -> Shutdown -> RemoveTask` 自等待。该顺序是 Component 生命周期
边界，不改变 MC-613 才负责的 `.so` 加载、回滚和 `dlclose` 所有权。

### 为什么 ComponentFactory::Instance 必须在核心库中定义？

函数局部静态对象若定义在头文件内，主程序和每个组件 `.so` 都可能得到各自的注册表，
dlopen 期间的静态注册器会写入插件私有表，mainboard 无法创建组件。MC-612 将
`ComponentFactory::Instance` 的定义移入 `minicyber_core`；组件仅引用该导出符号，配合
MC-613 要求的 `RTLD_GLOBAL` 后共享唯一表。真实 dlopen/dlclose 的加载水位回滚仍由
MC-613 验收，当前任务只固定其必要 ABI 前提。

### mainboard 为什么要求独立 Scheduler 配置且必须先创建 Scheduler？

`cyber_ref/cyber/mainboard/module_argument.cc` 将启动参数职责与模块加载分离。MC-613
把 MiniCyber 收敛为唯一 `-d` DAG 和唯一 `-s` Scheduler 文本配置：先解析 Protobuf，交给
`SchedulerFactory` 拒绝非法策略或空 Choreography 分区，随后才加载 DAG。这样同一业务 DAG
只替换 Classic/Choreography 配置，不会复制业务图或让 Component 初始化落到默认 Scheduler。

### dlclose 前为什么必须 Shutdown、销毁组件并注销工厂条目？

`cyber_ref` 的 ClassLoader 在组件生命周期结束后才卸载库。MC-613 的
`ModuleController::RollbackTo` 以本次加载水位为界，逆序执行 Shutdown、销毁 `shared_ptr`、
再 `dlclose`；插件注册器析构时从核心库唯一 `ComponentFactory` 注销 CreatorFunc，避免后续
Create 跳到已经卸载的代码段。测试 `.so` 记录 `I -> S -> D -> U`，并覆盖未知类失败时不伤害
先前水位。它不等同于 MC-615 的真实业务插件或 DAG。

### SIGINT/SIGTERM 为什么不能直接调用 C++ 清理逻辑？

信号处理器只能写 `sig_atomic_t` 通知，不能触碰 mutex、堆、日志、Transport 或 Scheduler。
MC-613 由主线程在通知后按 Component、Transport/Discovery、Scheduler 的顺序关闭，确保已卸载
`.so` 不会再被发现或传输回调触达。进程级测试在 Classic 和 Choreography 配置下均等待插件真实
初始化后发送 SIGTERM，并确认有界退出及 `I -> S -> D -> U` 顺序。

### 为什么 `sig_atomic_t + pause()` 仍然可能丢失退出信号？

`sig_atomic_t` 只保证标志的信号上下文读写安全，不会把“检查标志”和“进入
阻塞”合并为原子操作。信号在两者之间到达时，主线程会在标志已置位后仍然
`pause`。mainboard 在创建 Scheduler 线程前阻塞 SIGINT/SIGTERM，再由主线程以
`sigwait` 同步消费；初始化期已到达的信号保持 pending，等待期的信号也不会落在检查
与阻塞之间。这个选择还避免 POSIX SHM 的异常清理 handler 在异步 Channel Join 后覆盖
正常退出 handler；正常退出仍由主线程执行既定析构顺序，不引入退出线程。

### DataVisitor 回调为什么需要“未发布通知”挂账？

RoutineFactory 创建期间同时存在三个事件：安装 DataNotifier 回调、创建并首次
Resume CRoutine、发布 `task_id`。如果协程已进入 DATA_WAIT，但数据在任务号发布前
到达，直接忽略回调会让已填充 Buffer 失去对应的 READY 转换。因此回调在同一状态锁
下记录 pending，`task_id` 发布后补一次 `NotifyTask`。这不是新调度策略，而是完成
`DataNotifier -> DATA_WAIT/READY` 状态发布的并发契约。

### 绑核为什么必须检查 `pthread_setaffinity_np` 的返回值？

容器 cpuset、无效 CPU、目标线程已退出都可能让设置失败。忽略结果会使运行时声称
已绑核，实际却由内核自由迁移线程，Choreography 的线程归属证据和性能数据都会失真。
MiniCyber 不改变原生的 affinity 选择规则，只将系统调用成败变为可诊断事实；
测试则必须保持目标线程存活，避免把测试对象生命周期误判为调度策略错误。

## MC-616 多进程主干

### 为什么不把业务 DataDispatcher 显式实例化直接放进 minicyber_core？

跨 DSO 确实需要一份常驻的 DataDispatcher 状态和 shared_ptr 销毁入口，但这个需求
不意味着通用中间件核心应依赖 CameraFrame、ControlCommand 等具体业务类型。
MiniCyber 使用常驻的 `minicyber_autodrive_runtime` 持有业务模板实例：Source/Sink 和组件
插件都链接它，插件卸载后 Runtime 仍然存活；`minicyber_core` 继续只表达通用调度、
通信和生命周期职责。

### 为什么默认主干 Reader 队列不能只配置为 8？

`CacheBuffer` 是有界队列，消费者落后并超过活动窗口时，`ChannelBuffer::Fetch` 会快进
到最新消息。这是面向实时最新值的原生语义，不是无界可靠队列。但本项目的默认
业务验收又明确要求 1000 条无缺号，因此唯一 DAG 将业务队列设为 1024，覆盖验收
窗口。这是业务 QoS 配置，不修改中间件的覆盖策略，也不把 MiniCyber 宣传为可靠持久队列。

### 为什么只等 HasReader 与扩大队列仍不能保证第一条测量数据不丢？

HasReader 证明对端已 Join，不证明每层 Component 的 CRoutine 都已首次运行。
`ChannelBuffer::Fetch` 在消费游标为 0 时按 CyberRT 的最新值语义直接取 `Tail`；某层首次
运行前若已累积两条输入，第一条会被跳过，无论队列多大都一样。因此 MiniCyber
用 sequence 0 做端到端预热：Source 等到它从 ControlAudit 跨进程回执，再发 1..N
测量序列。这个屏障证明整条协程链已消费首次游标，同时不改变底层最新值语义。

### 为什么 VehicleState 必须预热，并且每个测量序列先于 CameraFrame 发布？

Fusion 是双通道 `AllLatest`：CameraFrame 为主通道，只有主通道到达时才会取 VehicleState
的当前最新值并推动业务协程。若第一帧 CameraFrame 先到，次通道尚未填充会使首个测量样本
没有可融合的 VehicleState；若同一序列颠倒顺序，则 CameraFrame 可能组合上一条次通道。
因此 SensorSource 先按 `VehicleState -> CameraFrame` 写 sequence 0，并等待它完整穿透五组件、
由 ControlAudit 回执形成端到端屏障；随后对每个测量序列保持相同顺序，并在全链路透传源序列和单调
时间。这是 `cyber_ref` 数据驱动访问器的主通道/最新值职责，不是时间戳同步或额外可靠性协议。

### 为什么 Source 发完不能立即销毁 Writer？

`Writer::Write` 成功只说明消息已进入当前传输后端，不代表独立 mainboard 中的 Component
协程链和外部 ControlSink 已完成尾部消费。若 Source 在最后一帧后立即关闭 Node，SHM
Transmitter 会 Disable，最后的输入块可能在 Perception、Fusion、Planning、Control 尚未完成时被
撤销，表现为稳定频率下的末尾缺号。MC-616 的启动脚本以 Sink 收到完整且连续的
ControlCommand 作为排空事实，再 SIGTERM 仍持有 Writer 的 Source，最后终止 mainboard。
这保持 Node 端点“Disable 后 Leave”的原有关闭职责；它不把发送确认伪装为 Transport 的可靠
投递能力，也不以增大 SHM 块数掩盖生命周期顺序。

### 为什么 Source 的两个输入 Reader Join 不足以放行整条管道？

它们只证明 SensorSource 可以向 Perception 和 Fusion 的输入端发送。ControlCommand 的
Writer 与先启动的 ControlSink Reader 通过独立的异步 Join 收敛；在 Writer 尚未观察到对端时，
既有 Hybrid API 允许 Write 成功返回但没有跨进程发送对象。MC-616 因此先让 ControlSink
以 `Reader::HasWriter` 观察这个下游 Join，再写入有界 ready 条件给启动脚本；脚本确认后才
启动 Source，Source 仍自行等待两个输入 Reader。这样每项放行都由对应的拓扑事实支撑，
不是用经验 sleep 延长启动时间。

## MC-617 高风险边界

### 为什么 `minicyber_core` 不能链接自动驾驶 Protobuf？

核心层的公开 Channel 边界是 `google::protobuf::Message`，不是 CameraFrame 或
ControlCommand 等唯一业务 DAG 类型。若把全部生成 Proto 整体链接进
`minicyber_core`，核心共享库会导出具体业务符号，业务模板实例和描述符的所有权也会变得
模糊。MC-617 将通用协议保留在 `minicyber_proto`，把 `autodrive.proto`、业务
`DataDispatcher` 显式实例和描述符放入常驻的 `minicyber_autodrive_runtime`；可卸载组件
只引用该 Runtime，因此不会在每个 `.so` 中复制进程内数据总线。

验证不依赖源码注释：Debug 构建后 `nm -D --defined-only libminicyber_core.so` 不包含六类
自动驾驶消息符号，而 Runtime 持有这些符号；`test_autodrive_proto` 和真实 dlopen 组件测试
仍通过。这不表示支持混用任意第三方 Protobuf ABI，所有主程序、核心、Runtime 与组件仍需
使用同一套 Protobuf ABI。

### 为什么关闭 metrics 后仍保留序列集合，却不能保留延迟样本？

ControlSink 的序列集合用于丢包、重复、乱序及 Audit 一致性这些业务完成断言，即使不输出
性能数据也必须存在。逐消息延迟样本只服务 p50/p95/p99；在 `--no-metrics` 时保存它们会引入
没有业务价值的分配和排序输入。MC-617 将记录逻辑收敛到独立的 Metrics 状态，并由
`test_control_sink_metrics` 断言关闭时连续接收消息后 `latency_ns` 始终为空；开启时才保留
有效的单调时钟差值。正式 Release 性能结论仍只属于 MC-620。

### 为什么功能 evidence 必须与性能 metrics 使用两个独立开关？

metrics 负责端到端延迟、吞吐和完整性统计，是被测业务结果的一部分；MC-619 evidence
则为了证明 Processor 线程归属、INTRA 指针身份、SHM 进程边界和逐组件序列集合，会执行
额外的互斥、容器插入和地址记录。如果两者共用启动条件，性能样本会把功能取证成本计算
进中间件延迟，结论无法说明关闭调试探针后的主干表现。因此统一脚本默认关闭 evidence，
功能验收显式开启，性能采集只开启 metrics。隔离开关不是优化传输算法，而是保证测量对象
和验收探针边界清晰。
