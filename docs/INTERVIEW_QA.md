# MiniCyber 面试问答

> 本文是 MC-602 的初稿。回答以第二次重构的定稿边界为准，并明确区分首轮实现、
> 后续任务和已验证能力。

## 项目定位

### MiniCyber 要解决什么问题？

MiniCyber 是 Linux x86_64、C++17 和 POSIX SHM 上的 CyberRT 精简实践。最终用一条
自动驾驶消息链验证栈式协程、两种 Scheduler、Node/Reader/Writer、INTRA、Protobuf
SHM、发现控制面、动态组件和优雅退出如何协作，而不是实现完整 Apollo 或通用自动驾驶
平台。

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
进入 Classic 公共池。同一 DAG 只替换 Scheduler 配置，不复制业务链。最终共享队列
和公共池由 MC-610、MC-611 验证。

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
分档，而是让正式 Protobuf Reader/Writer 都请求 64 KiB/4 块，并在既存布局更小时明确
初始化失败。它解决确定性和可诊断性，但不宣传动态扩容能力。

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
工作区状态。不得将已删除的 PIPE、互斥队列、ping-pong 或 C++20 探针数值拿来比较，
也不得从单次测量推出普适优势。

## 项目不具备的能力

MiniCyber 不支持 Windows、aarch64、跨主机 RTPS 数据、RPC、TimerComponent、录包
回放、参数服务、监控、Python 接口或旧离散 Demo。FastRTPS CMake 包和完整业务主干
尚未完成时，也不把计划中的能力说成已实现。

## 证据来源

本初稿迁移自首轮模块映射、调度/传输/组件笔记、旧性能报告与 00 历史记录；具体最终
决策以 `docs/refactor/02_架构取舍矩阵.md`、验收以
`docs/refactor/03_验收与性能口径.md` 为准。MC-622 将在完整主干有真实证据后扩写。

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
