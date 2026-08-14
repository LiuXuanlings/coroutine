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

### INTRA 和 SHM 的零拷贝边界是什么？

INTRA 只在同进程传递同一个 `shared_ptr`，可证明对象身份不变。跨进程不能共享带有
动态内存和虚表语义的 C++ 对象，SHM 必须写入 Protobuf 序列化字节并在接收端构造新
消息。对外 Channel API 因此只接收 `google::protobuf::Message` 派生类型。

### 为什么需要 HybridTransport？

同一个 Writer 可能同时面对本进程 Audit Reader 和外部 Sink Reader。最终
HybridTransport 按每个对端并行维护连接：同进程走 INTRA，其他同机进程走 SHM；它
不是创建端的一次性全局二选一，也不允许重复投递。该能力待 MC-607 至 MC-608 完成。

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
双向跨进程 Join/Leave 证据。HybridTransport、Protobuf-only Node 和完整业务链仍分别属于
MC-607 以后，不能把这些计划能力提前说成已实现。

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
