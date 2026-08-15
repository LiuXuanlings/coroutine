# MiniCyber 调试手册

> 本文由 MC-602 从首轮实施资料迁移而来。它只记录对后续重构仍有约束力的历史
> 事实；当前架构边界以 `docs/refactor/02_架构取舍矩阵.md` 为准。

## 一、使用原则

- 先确认问题位于协程、数据唤醒、调度、传输还是动态加载边界，再缩小到对应测试
  和进程。
- 优先使用可关闭的日志、`gdb`、`strace`、ASAN 和现有测试；不要在信号处理器、
  回调或锁保护区加入复杂临时代码来证明猜想。
- 回归必须覆盖正常路径和关闭路径。旧基准数值、已删除能力和未验证的性能结论都
  不能作为诊断依据。

## 二、协程上下文与栈上引用

### x86_64 栈对齐和上下文切换

**症状**：首次或往返恢复协程时崩溃、寄存器异常，或只在优化构建出现不稳定。

**已验证事实**：MC-102 对齐了 `src/swap.S`、
`src/croutine/detail/routine_context.cpp` 与 CyberRT 的 x86_64 上下文职责，
并以 `tests/test_croutine.cpp` 覆盖双向切换。实现只承诺 Linux x86_64，不能推断
aarch64 或其他平台可用。

**定位和回归**：在 `minicyber::CRoutine::Resume`、`SwapContext` 和协程入口处用
`gdb` 检查栈指针与返回地址；先运行下列测试，再判断是否只在具体 Processor 或关闭
路径发生。不能通过跳过第二次恢复或改变保存寄存器集合来掩盖 ABI 问题。

```bash
cmake --build build/debug -j2 --target test_croutine
ctest --test-dir build/debug --output-on-failure -R '^test_croutine$'
```

### MainFunc 栈冻结导致的 `shared_ptr` 泄漏

**症状**：协程执行完回调后不再恢复，ASAN 或长期运行显示 `CRoutine` 和其栈仍被
持有；测试若只执行一次 `Resume()` 也会留下对象。

**根因**：协程栈在 `Yield` 后被冻结。入口函数若在这个栈上调用 `GetThis()`，临时
`shared_ptr` 的析构永远不会执行，引用计数持续持有协程对象。这不是
`shared_ptr(this)` 的双控制块问题，而是栈帧没有机会析构。

**已验证修复边界**：`src/croutine.cpp` 的 `CRoutine::MainFunc` 先保存当前协程，
清空回调，取得裸指针，并在 `Yield` 前释放局部 `shared_ptr`。`CRoutine::Yield`
同样只用裸指针切回主协程。测试必须继续恢复到 `FINISHED`，不能把未完成协程当作
测试结束状态。

### CRoutine 与时间库的 `Duration` 名称冲突

**症状**：同一翻译单元包含 `minicyber/croutine/croutine.h` 和旧版
`minicyber/time/time.h` 时，编译器报告 `minicyber::Duration` 重定义，或后续代码把
协程的微秒别名误当作纳秒时间间隔。

**根因和修复边界**：协程保留 `minicyber::Duration` 作为内部等待时长别名；旧时间库
也在根命名空间定义了不同语义的 `Duration`。MC-605 将 Time、Duration、Rate 收拢到
`minicyber::time`，以 `time::Duration` 表示纳秒间隔，避免修改协程状态与睡眠职责。
业务节拍只能使用 `time::Rate`，不能以该修复为理由把 Rate 接入 Scheduler 或恢复
Timer。

**定位和回归**：确认调用方包含 `minicyber/time/rate.h` 并使用 `minicyber::time`；
`tests/test_time.cpp` 同时包含协程和时间头文件，覆盖类型隔离、`MonoTime`、
`SleepUntil` 与 Rate 超期后的周期重置。

```bash
cmake --build build/debug -j2 --target test_time
ctest --test-dir build/debug --output-on-failure -R '^test_time$'
```

## 三、数据通知和 AllLatest

### Notifier 注销竞态

**症状**：Reader、Visitor 或 Component 关闭后仍进入回调，偶发 UAF、死锁或关闭
卡住。

**已验证事实**：首轮 MC-202/MC-203 让 Dispatcher 在安全快照上分发，并让
`DataNotifier::RemoveNotifier` 先从注册表移除、再停用并等待已在执行的回调结束。
后续 MC-609/MC-612 改造必须保留这一注销屏障；关闭时先阻止新任务，再移除调度任务
和 Reader。

### AllLatest 填充必须先于唤醒

**症状**：双输入组件在主通道到达后被唤醒，却看到空融合缓冲并再次等待，直到下一
条主通道消息才恢复。

**根因和约束**：双通道由主通道驱动。`AllLatest` 的填充回调必须先注册，随后才注册
唤醒协程的 notifier；`DataNotifier` 按注册顺序通知，协程被唤醒前已有融合结果。
游标只由融合器推进一次，Visitor 不得再次递增。次通道没有最新值时不应伪造融合
成功。

**定位路径**：检查 `include/minicyber/data/data_visitor.h` 的双通道构造顺序和
`include/minicyber/data/fusion/all_latest.h` 的主通道语义。当前三、四输入路径待
MC-609 删除，不能作为最终 API 的调试依据。

## 四、POSIX SHM 异常清理

**症状**：`SIGINT`、`SIGTERM` 或崩溃后 `/dev/shm` 残留 `minicyber_*`，后续运行
因同名段或不一致的段参数失败。

**根因**：内核会回收进程映射和文件描述符，但不会自动执行命名共享内存的
`shm_unlink`。正常 RAII 析构无法覆盖异常终止。

**约束**：创建者在段创建成功后登记名称，纯消费者不负责删除创建者的段；正常销毁
走 Close、注销和 `shm_unlink`。信号路径只读取固定登记表并调用异步信号安全的
`shm_unlink`，恢复默认处理后重新抛出信号。不得在信号处理器中调用 `Destroy`、
加 `std::mutex`、分配内存或使用 `std::string`。

```bash
cmake --build build/debug -j2 --target test_posix_segment_signal
ctest --test-dir build/debug --output-on-failure -R '^test_posix_segment_signal$'
```

运行前后应比较 `/dev/shm/minicyber_*`。跨进程 Protobuf SHM 路径将在 MC-607 和
MC-617 重新覆盖。

## 五、FastRTPS 发现控制面

### CDR 对齐导致 ChangeMsg 反序列化失败

**症状**：两个同机进程已创建 Participant，但 Reader 迟迟看不到远端 Writer，FastRTPS
输出 `Deserialization of data failed`。直接把 Protobuf 字节写入 FastRTPS payload 时，问题
会随消息长度出现或消失。

**根因和修复边界**：FastRTPS 的 `TopicDataType` payload 是 DDS CDR 表示，不是裸字节
容器；传输层会按 CDR 边界补齐。若裸 Protobuf 尾部混入补齐的 `0x00`，Protobuf 会将其
视为非法字段标记而解析失败。MC-606 的私有 `ChangeMsgType` 用 `DDS_CDR` 封装和长度明确
的字节序列承载 Protobuf，再交给 `ChangeMsg::ParseFromArray`。这只适用于 Join/Leave
控制 Topic，不能据此恢复 RTPS 数据面或公开 RTPS 类型。

**Participant 离场**：Participant 名称携带本机 host 和 PID。发现监听到远端 Participant
被移除或丢弃时，TopologyManager 从唯一 writer/reader 状态表删除该进程的角色，并向变更
监听器发送对应 Channel Leave。关闭时先移除 Participant，随后清空状态和监听器，避免
已卸载消费者收到后续控制面回调。

**定位和回归**：确认 `RoleAttributes.host_name` 与本机一致，检查控制 Topic 的 CDR
封装和字节序列长度；不要用固定 sleep 代替 `HasReader`/`HasWriter`。跨进程测试覆盖
Writer/Reader Join、Leave 和状态恢复。

```bash
cmake --build build/debug -j2 --target test_topology test_topology_discovery
ctest --test-dir build/debug --output-on-failure -R '^test_(topology|topology_discovery)$'
```

### MC-606 发现时序问题完整排查实录

**触发环境与最小复现**：Linux x86_64、FastRTPS 2.5，同一 GoogleTest 中 fork Writer
和 Reader 两个进程。原测试在 Participant 启动后固定等待 800 ms，再让两端 Join；质量
评审将断言改为双向发现、`DIFF_PROC` 和显式 Leave 后，执行重复测试：

```bash
ctest --test-dir build/debug --output-on-failure \
  -R '^test_(topology|topology_discovery)$' --repeat until-fail:10
```

**原始现象**：失败时 Reader 退出码 11，表示从未在稳定状态观察到 Writer Join；Writer
随后退出码 13，表示一直等待 Reader Leave。单次运行可能通过，因此不能用“偶尔通过”
作为完成证据。

**排查时间线**：

1. 首先核对 `cyber_ref/cyber/transport/qos/qos_profile_conf.cc`，发现原生
   `QOS_PROFILE_TOPO_CHANGE` 是 Reliable、Transient Local、Keep All、depth 10；当前实现
   使用 FastRTPS 默认 QoS，晚加入端点没有可靠重放保证。
2. 先补 Reliable 和 Transient Local。候选假设是“只缺可靠性和持久性”；重复测试仍会
   出现 Join 很快被 Leave 覆盖，证明该假设不完整。
3. 补齐 Keep All(depth=10)，排除历史缓存只保留最终状态的风险；双进程同时 Join 时仍有
   偶发失败，说明 QoS 是必要条件，但测试仍把端点匹配完成误当成 `Start()` 的同步后置条件。
4. 删除固定 800 ms，增加管道事件：Reader 完成本地 Join 后通知父进程，父进程再允许
   Writer Join。这消除了“进程已启动”等于“控制端点已匹配”的错误假设。
5. 此时仍出现 Reader 在一次 100 ms 轮询之间连续处理 Writer Join 和 Leave，最终表中已无
   Writer。检查退出码 11/13 后确认这不是 Join 丢失，而是测试只采样最终状态，遗漏短暂事件。
6. 在 Reader Join 前注册 ChangeListener，记录 Writer Join/Leave；Reader 观察到 Join 并
   校验 `HasWriter` 与 `DIFF_PROC` 后，通过第二条管道确认 Writer 才允许 Leave。最终状态和
   事件序列均被确定性验证。

**工具与关键证据**：构建时受限环境的 ccache 默认目录只读，使用
`CCACHE_DIR=/tmp/minicyber-ccache` 和 `CCACHE_TEMPDIR=/tmp/minicyber-ccache-tmp`；沙箱内
FastRTPS 曾报告 `getifaddrs/open: Operation not permitted`，这是网络接口权限限制，必须在
允许访问本机接口的环境复跑，不能归因为产品代码。最终 `test_topology_discovery` 连续 10
轮通过，单轮约 0.66 至 0.97 秒。

**根因与修复边界**：根因由两部分组成：控制 Topic 未显式对齐原生 QoS，以及测试缺少
跨进程事件握手、把异步事件压缩成一次最终状态轮询。修复还过滤本进程 FastRTPS 回环，
避免本地 `OnChange` 与控制消息自回环产生重复通知。没有引入 RTPS 数据面，也没有用扩大
超时或固定 sleep 掩盖竞态。

**剩余回归责任**：当前覆盖双向 Join、`DIFF_PROC`、显式 Leave、回环去重和监听回收；
Participant 异常退出、晚加入、启动失败回滚及监听器并发注销由 MC-617 专项收口。

**可迁移经验**：异步发现测试要分别证明“事件确实发生”和“最终状态正确”。启动完成、
端点匹配完成、历史样本重放完成是三个不同阶段，应使用有界事件协议表达因果关系。

## 六、调度启动、关闭和动态库

### Hybrid 状态锁与同步 INTRA 回调形成自死锁

**触发环境与最小复现**：MC-609 后的 Debug 构建中，创建同进程 Hybrid Writer/Reader，
Reader 回调内调用同一 Writer 的 `Disable`。原实现的 `HybridTransmitter::Transmit` 持有
Hybrid `mutex_` 调用 `IntraTransmitter::Transmit`，INTRA 又在发布线程同步进入该回调。

**原始现象与排查时间线**：静态调用链显示回调内关闭会沿
`Transmit -> DataDispatcher -> DataNotifier -> Reader callback -> Disable` 回入同一非递归锁。
首先对照 `IntraReceiver::Disable`，确认底层已经刻意在生命周期锁外等待 in-flight，排除了
底层 Notifier 必然死锁的假设；随后检查 HybridReceiver，发现其仍在 Hybrid 锁内调用可能
等待回调的后端 Disable，说明问题位于新增聚合层。若只把 `Transmit` 解锁，拓扑 Leave 与
关闭并发时仍可能在 `ReconcileBackendsLocked` 中重现等待环，因此最终统一拆分为“锁内更新
期望状态、锁外启停后端”。

**工具与证据**：使用 `rg`、`nl` 对照 Hybrid、IntraReceiver、DataNotifier 的锁和回调链；
新增 `IntraCallbackCanDisableTransmitter` 与 `IntraCallbackCanDisableReceiver`，定向重复测试
验证回调能够有界返回。协调循环用原子标志串行消费期望状态；回调内再次关闭只更新状态，
不等待当前正在执行的后端操作。Receiver 另以 `accepting_` 在关闭开始时阻止新用户回调。

**根因、修复边界和经验**：根因不是“互斥锁不能使用”，而是持生命周期锁调用了可同步
回入用户代码或等待用户代码结束的路径。修复未改变 Hybrid per-opposite 路由，只调整锁
边界。以后审查聚合层时，不能因为底层类已实现安全注销，就假设上层持锁调用底层仍安全。

### SHM 段容量随端点启动顺序变化

**触发环境与最小复现**：Protobuf `ShmTransmitter` 默认请求 64 KiB，但 Receiver 先启动时
`ShmDispatcher::AddSegment` 原来使用 `PosixSegment` 的 1 KiB 默认值创建同名段。Writer 后
启动进入 `OpenOnly`，无条件采用已有布局；超过 1 KiB 的动态字段随后申请块失败。

**排查时间线**：先比较 Sender、Dispatcher 和 PosixSegment 三处默认参数，确认不是
Protobuf 序列化失败；再检查 `OpenOnly`，发现它校验布局后覆盖调用方请求值。对照
`cyber_ref/cyber/transport/shm/segment.cc` 和 `shm_conf.cc`，原生实现会按消息大小档位管理
容量并在不足时重建。MiniCyber 不恢复完整分级机制，但正式 Protobuf 两端必须请求相同
布局，较小既存段必须明确拒绝。排查还发现 `AddListener` 在 `AddSegment` 失败后仍返回有效
ID，会造成 Receiver 表面启用但实际没有可读段。

**修复与回归**：`AddSegment` 改为返回结果并校验既存段容量；Protobuf `AddListener` 默认
请求 64 KiB/4 块，失败返回 0；`OpenOnly` 拒绝小于请求值的布局。Hybrid SHM 测试使用
2048 字节动态字段覆盖 Reader 先启动路径，PosixSegment 测试覆盖 1 KiB 既存段拒绝 64 KiB
请求。该修复只做一致布局和失败诊断，不宣称具备原生动态扩容能力。

### 未确认问题：ConditionNotifier 跨进程偶发失败

MC-608 全量测试曾出现 `ConditionNotifierTest.ForkCrossProcessNotify` 子进程状态 1，随后
连续 5 轮通过，当前没有稳定根因。该现象不能因复跑通过而删除：MC-617 必须记录复现率、
父子进程退出码、notifier SHM 初始状态和清理结果，并用事件条件而非扩大 sleep 判断是否为
通知初始化、残留资源或测试编排问题。在 MC-617 收口前，它保持“未确认问题”状态。

### MC-611 旧测试对象 ABI 不一致导致的 `stack smashing`

**触发环境与最小复现**：扩展 `SchedulerConf` 头布局、但只增量重编译
`test_scheduler_choreography` 和 `test_scheduler_classic` 后，执行高风险调度门禁：

```bash
ctest --test-dir build/debug --output-on-failure --repeat until-fail:20 \
  -R '^test_(scheduler_choreography|scheduler_classic|processor|classic_context|routine_factory)$'
```

首次运行在 `test_routine_factory` 退出码 8 时出现 `*** stack smashing detected ***`。
该轮 5 项中 1 项失败，失败率为 1/1；异常发生在旧对象仍按扩展前的
`SchedulerConf` 大小和布局访问栈对象时。

**排查步骤与证据**：对比公共头和目标文件后，确认 `test_routine_factory` 对象没有因
头布局变更而重编译；随后执行完整 Debug 构建及定向复跑：

```bash
cmake --build build/debug -j2
ctest --test-dir build/debug --output-on-failure --repeat until-fail:20 \
  -R '^test_routine_factory$'
```

完整重编译退出码 0，`test_routine_factory` 随后连续 20/20 通过。没有观察到同一失败
在新对象上的复现，因此该条目保持“未确认问题”而非产品逻辑回归；归属为 MC-611
构建/ABI 收口，后续任务不得把它当作调度运行时故障。

**根因、修复边界与经验**：根因是公共配置结构扩展后的陈旧测试对象 ABI 不一致，
不是 Scheduler 线程安全或关闭顺序。修复边界仅是完整重编译受影响目标，没有加入
运行时兼容层或改变 `SchedulerConf` 语义。修改公开头文件中的结构布局后，必须在高风险
复跑前执行完整构建，避免旧对象污染结果。

**构造期虚调用历史问题**：旧 IOManager 在基类构造期间启动工作线程，线程进入
基类 `run()` 而非派生类实现；停止时主线程卡在 `pthread_join`，工作线程等待
`futex` 或 `epoll_wait`。根因是构造和析构期间虚函数表只代表当前层级。

**定位工具**：`strace -f` 先定位阻塞系统调用；`gdb` 的 `info threads`、`thread`
和 `bt` 确认实际入口。原则是派生对象构造完整后再启动可能虚调的线程，并在关闭时
唤醒全部等待者。IOManager 已不属于最终范围；MC-610/MC-611 将按新的 Scheduler
语义重新验证关闭竞态。

**动态加载**：首轮 `ModuleController` 的失败回滚以进入时的组件和库水位为界，只
逆序关闭、销毁和卸载本次新增资源。排查顺序为库路径、`dlerror()`、静态注册是否
进入唯一工厂、组件初始化、回滚水位和 Shutdown 顺序。MC-613 会移除空
`module_library` 的生产绕过路径。

### MC-612 将 Component 改为协程执行后遗留同步断言

**触发环境与最小复现**：完成 Component 的 `DataVisitor -> RoutineFactory -> Scheduler`
接入后，执行：

```bash
ctest --test-dir build/debug --output-on-failure \
  -R '^(test_component|test_component_base|test_component_factory|test_module_controller)$'
```

`test_module_controller` 的 `EndToEndDataDelivery` 首次失败，CTest 退出码为 8；4 项中 1 项
失败，失败率 1/1。失败断言是 Writer::Write 返回后 `McTestComponent::proc_count()` 仍为 0，
旧测试却期待同步等于 1。

**排查时间线与候选假设**：先重建 Component、Scheduler 与 ModuleController 目标，排除头
文件修改后的陈旧对象 ABI；再检查 `Writer::Write`、INTRA Dispatcher、DataNotifier 和
RoutineFactory 调用链，确认发布线程现在只执行 `Dispatch -> NotifyTask`，而 Proc 必须等待
Processor 从 `DATA_WAIT` 转为 READY 后 Resume。随后检查 Component 测试，Proc 所在线程与
发布线程不同且双输入首通道驱动均已通过，排除消息未进入 DataVisitor 或 Scheduler 未启动。

**根因、修复与回归**：根因是 ModuleController 测试沿用了 MC-612 前的同步 Proc 语义，不是
数据丢失或调度死锁。将断言改为两秒有界的条件等待，不使用固定 sleep；生产 Component
行为没有增加同步回退。修复后上述 4 项测试全部通过，且：

```bash
ctest --test-dir build/debug --output-on-failure --repeat until-fail:20 \
  -R '^test_component$'
```

连续 20 轮通过。后续 MC-617 应保留这条“发布线程不等于 Proc 线程”和回调内关闭门禁；不得
再次把 Write 返回时点当作业务处理完成时点。

### MC-612 的 SHM Component 分发误投递到 INTRA 回调

**触发环境与最小复现**：为使跨进程 SHM 解码后的 Protobuf 进入 Component 的
DataVisitor，最初直接调用通用 `DataDispatcher<M>::Dispatch`。在稳定 Debug 构建执行
`ctest --test-dir build/debug --output-on-failure` 时，
`HybridTransportTest.MixedOppositesFanOutWithoutDuplicateDelivery` 失败；测试进程退出码为
8，8 项中 1 项失败，失败率 1/1，完整集为 43/44。

**原始现象与排查时间线**：本地 Hybrid Reader 收到了 SHM 解码产生的新 `shared_ptr`，而非
Writer 原始对象，计数从期望的 1 变为 2。先检查 Hybrid 的 per-opposite 连接集合，确认
同机 Reader 和跨进程 Reader 的连接关系没有重复建立；再追踪 `ShmReceiver ->
DataDispatcher -> DataNotifier`，发现通用 Dispatcher 同时持有 IntraReceiver 的 Buffer 与
DataVisitor Buffer，SHM 副本因此被错误送回本进程 INTRA 回调。候选“SHM 自身重复通知”被
指针身份和该静态路由证据排除。

**修复边界与回归**：DataDispatcher/DataNotifier 增加仅由 DataVisitor 显式登记的 SHM
投递分支；ShmReceiver 反序列化后只填充这些 Visitor Buffer 并唤醒协程，普通 Reader 仍仅由
ShmReceiver 的原始回调投递一次。修复没有改变 Hybrid per-opposite 路由、INTRA 零拷贝或
SHM Protobuf 边界。`test_component` 与 `test_hybrid_transport` 各连续 5 轮通过，随后在没有
并行构建的稳定产物上完整 CTest 44/44 通过。经验是不能因为 Component 需要接收 SHM，就把
已解码副本注入 INTRA 的普通端点总线；数据协程 Buffer 与 Transport 回调必须维持独立订阅
类别。

### 动态库卸载后残留 ComponentFactory 创建函数

**风险与最小复现**：若 `.so` 的静态注册器仅在 `dlopen` 时向核心工厂登记 CreatorFunc，
`dlclose` 后注册表仍可能保存指向已卸载代码段的 lambda。下一次 `Create` 会跳转到无效地址；
若先 `dlclose` 再销毁 Component，虚析构和 `Shutdown` 也会落入已卸载库。

**定位与修复边界**：检查 `ModuleController::RollbackTo` 的水位、组件销毁位置和
`ComponentFactory` 注册表。MC-613 将回滚固定为“逆序 Shutdown -> 销毁 shared_ptr -> 逆序
dlclose”，并让注册器析构时注销类名；核心工厂进程常驻，避免 dlclose 时的跨 DSO 静态析构顺序。
这不实现热重载或并发卸载，只保证 mainboard 的串行加载边界。

**回归命令与结果**：

```bash
cmake --build build/debug -j2 --target mainboard test_module_controller
ctest --test-dir build/debug --output-on-failure -R '^test_module_controller$'
```

测试构建最小插件 `.so`，断言真实注册创建及 `I -> S -> D -> U`；未知类加载失败仅回滚
本次水位，已加载组件保持可用。进程级用同一 DAG 搭配 Classic/Choreography 配置，等待初始化
痕迹后发送 SIGTERM，两个子进程均退出码 0。

### MC-613 质量评估：mainboard 单次退出信号丢失窗口

**风险现象与复现思路**：原 `WaitForShutdown` 执行 `while (flag == 0) pause()`。
如果 SIGINT/SIGTERM 在循环判断完成后、`pause` 进入内核前到达，处理器已将
`sig_atomic_t` 置位，但主线程随后仍会休眠并等待第二个信号。这是代码时序可
证明的 lost wakeup，普通的“初始化后再 kill”测试不易命中。

**排查路径**：先确认信号处理器只写 `sig_atomic_t`，排除处理器内 mutex/日志导致
的异步信号不安全；再按“检查标志 -> 信号到达 -> pause”排列时序，确认缺少
原子的“解除阻塞并等待”操作。最后检查 Scheduler 线程创建时机，确认退出信号
必须在创建工作线程之前阻塞，使新线程继承掩码，避免信号被非主线程消费。

**修正与回归**：mainboard 在 Scheduler 创建前用 `pthread_sigmask` 阻塞 SIGINT/SIGTERM，
主线程使用 `sigsuspend` 原子解除阻塞并等待，处理器仍只写标志。测试插件在
`Initialize` 内主动产生 SIGTERM，证明信号先于等待到达时仍能触发
`I -> S -> D -> U` 和有界退出。

### MC-610 质量评估：RoutineFactory 任务号发布前丢失首条唤醒

**风险时序**：Scheduler 原先先向 DataVisitor 安装回调，再创建并入队 CRoutine，
最后才把 `task_id` 写入回调状态。Processor 可在写入之前将无数据协程挂到
`DATA_WAIT`；若首条消息恰在此时到达，回调因 `task_id == 0` 返回，Buffer 已有
数据但协程只能等待下一条通知。高频数据源会用下一帧掩盖问题，单条或低频
通道则可能永久停顿。

**定位与修正**：沿 `DataNotifier -> RegisterNotifyCallback -> RoutineWakeState ->
NotifyTask` 检查所有状态发布点，确认 Buffer 填充本身没有丢失，丢失的是状态转换通知。
回调现在于同一 mutex 下登记 `notification_pending`；发布任务号时取走挂账并立即
补一次 `NotifyTask`。无论“通知先”或“任务号先”，至少一条路径会完成唤醒。
MC-617 还必须保留专门的首条消息并发门禁，不能只用连续高频消息证明无丢失。

### MC-613 质量评估：绑核测试时序与 SHM 测试残留

**原始证据**：评估时 Debug 全量 CTest 为 43/44，
`PinThreadTest.SetSchedAffinity1to1` 失败。目标 `std::thread` 的函数体为空，
`pthread_setaffinity_np` 执行时线程可能已退出；生产封装又忽略返回值，导致测试
无法区分时序失败和宿主拒绝。同次全量测试结束后，
`find /dev/shm -maxdepth 1 -name 'minicyber_*'` 稳定发现 `minicyber_91006`；
`CloseIsIdempotent` 验证了 Close 两次，却没有完成测试所有权下的最终清理。

**修正边界**：绑核测试使目标线程存活到亲和性读回完成；`SetSchedAffinity`
返回系统调用结果，Scheduler 在配置未生效时输出警告，不改变原生的 affinity
分配语义。SHM 用例保留 `Close` 不删全局段的生产契约，但在断言后清理该测试
的精确名称，不再依赖人工删除。

## 七、证据来源

本初稿迁移自首轮 `00_进度记录.md`、`baseline.md`、`module_mapping.md`、
`croutine/shared_from_this.md`、`scheduler/debug_vtable_hang.md`、
`transport/signal.md` 及相关测试。旧文件已在 MC-602 删除；后续任务必须以现存
源码、02/03 定稿和新的验证结果更新本文。
