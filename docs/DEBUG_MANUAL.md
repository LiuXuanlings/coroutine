# MiniCyber 调试手册

> 本文由 MC-602 迁移历史事实，并由 MC-622 按第二次重构的真实故障、命令和回归证据
> 定稿。当前架构边界以 `docs/refactor/02_架构取舍矩阵.md` 为准；没有实际复现或测量
> 证据的推测不得写成故障结论。

## MC-619 Choreography 验收取证的构建与触达误判

### 触发环境与最小复现

Linux x86_64、GNU 11.4、Debug 构建。执行：

```bash
ctest --test-dir build/debug --output-on-failure \
  -R '^test_autodrive_choreography_pipeline$'
```

首次执行退出码 8，失败率 1/1，启动脚本报 `ControlSink did not observe
ControlCommand writer readiness.`。随后单独构建 `control_sink` 的命令退出码 2，编译器
指出 `Metrics` 的 `unordered_set` 不能绑定到取证代码要求的 `set` 引用。修复该编译错误后，
同一 CTest 又以退出码 8 失败 1/1，但主干已完成 1000 条收发；报告的失败行为是
`Untouched production file: include/minicyber/base/atomic_hash_map.h`。

coverage 配置首次执行：

```bash
cmake -S . -B build/coverage -DCMAKE_BUILD_TYPE=Debug \
  -DMINICYBER_ENABLE_COVERAGE=ON
```

退出码 1。外部 `https://kkgithub.com/google/googletest.git` 在三次 clone 中分别出现 TLS
握手终止和仓库不存在，未进入业务测试。

### 根因、修复边界与回归

第一个失败不是发现或 SHM 语义回归，而是新增 `--evidence-file` 尚未编入旧 Sink 二进制；
因此旧程序按用法退出，脚本等待不到 ready 文件。取证写出时将无序集合复制、排序后再导出，
不修改 `Metrics` 的集合语义。第二个失败证明“文本直接 include”不足以代表静态调用链：审计改读
`minicyber_core`、业务 Runtime/组件和三个业务进程目标的编译 `.d` 依赖，测试专用包含不计入。
据此删除五个只被旧测试引用、没有生产依赖的遗留头文件及三个对应测试，不能用测试伪造业务触达。

coverage 改为显式使用已验证的本地 GoogleTest 源目录
`/tmp/minicyber-googletest2`，不更改 CMake 的系统依赖边界。最终回归为 Debug Choreography
连续 3/3 通过；coverage Choreography 通过，`file_touch_report.txt` 记录
`gcov_data_files=38` 与 `gcov_scheduler=passed`。每轮均确认四个自动驾驶频道自然回收，命令
`find /dev/shm -maxdepth 1 -name 'minicyber_*' -print` 无输出。跨进程验收失败必须先确认实际
二进制已重新链接，再从运行时日志判断发现、调度或 SHM 根因。

## MC-615 业务 DSO 重复注册 Protobuf 描述符

### 触发环境与最小复现

在 Debug 构建中运行 `test_autodrive_components`，由测试进程通过
`ModuleController::LoadModule` 真实 `dlopen` `libminicyber_autodrive_components.so`。
业务库最初直接链接 `minicyber_proto`，随后即使移除直接依赖，核心库的静态 Proto
传递依赖仍可能按需把新消息对象拉入插件。

### 原始现象、排查与证据

1. `cmake --build build/debug -j2 --target minicyber_autodrive_components test_autodrive_components`
   退出码 0，说明组件代码、注册宏和链接符号均可生成。
2. `ctest --test-dir build/debug --output-on-failure -R '^test_autodrive_components$'`
   退出码 8；`dlopen` 后立即输出 `File already exists in database:
   minicyber/proto/autodrive.proto`，随后 `GeneratedDatabase()->Add` 触发 FATAL。失败发生在
   `ComponentFactory::Create` 和 `Component::Initialize` 之前，排除了 DAG 类名、TextFormat
   配置和业务 Proc 算法错误。
3. 对比 MC-613 的 `tests/module_controller_test_plugin.cpp`，该插件只链接
   `minicyber_core`。移除业务库的直接 Proto 依赖后再次复现，检查 `link.txt` 发现
   `minicyber_core` 的 `PUBLIC minicyber_proto` 仍把静态归档传给插件；因为核心此前没有引用
   `autodrive.pb.cc`，归档的按需抽取仍把该描述符复制进插件。

### 根因、修复边界与回归

Protobuf 生成文件属于核心库的唯一 ABI 所有者；mainboard、测试进程和业务插件必须共享
`minicyber_core` 中的那份描述符注册。业务 DSO 只链接 `minicyber_core`，核心以 whole-archive
收纳完整 `minicyber_proto`，继续使用其公开包含目录，不改变 ComponentFactory 的
`RTLD_GLOBAL` 注册边界，也不把组件静态链接进 mainboard。回归必须先确认五类组件全部注册，再验证 TextFormat 配置和业务链路；任何在
`dlopen` 阶段出现的 descriptor duplicate 都应先检查 DSO 是否复制链接了核心 Protobuf
静态库，不能修改消息编号或绕过动态加载。

## MC-615 跨 DSO 数据总线与真实卸载

### 触发环境与最小复现

同一 Debug 环境继续运行
`ctest --test-dir build/debug --output-on-failure -R '^test_autodrive_components$'`。
测试经 `ModuleController` 加载业务 `.so`，发布 VehicleState 与 CameraFrame，随后关闭
Node、组件和动态库；因此同时覆盖模板数据总线、静态注册器和 `dlclose` 生命周期。

### 原始现象与排查时间线

1. 描述符修复后第 2 次运行仍以退出码 8 失败，五个组件均已注册，但完整链路在 2 秒
   等待超时，失败率为 1/1。检查 `DataDispatcher<T>::Instance()` 后确认主程序和插件各自
   实例化了业务类型的函数局部静态数据总线，消息只进入发布方所在副本。
2. 在 `minicyber_core` 显式实例化六类业务 `DataDispatcher`，并在插件侧使用
   `extern template` 后，第 3 次运行链路通过，但 `controller.Clear()` 后
   `ComponentFactory::Has("PerceptionComponent")` 仍为真，命令退出码 8、失败率 1/1。
   `readelf -sW libminicyber_autodrive_components.so` 显示模板 Dispatcher、ShmDispatcher 和
   IntraDispatcher 的 `STB_GNU_UNIQUE` 符号；glibc 因此将该库保留为 `NODELETE`，不执行
   注册器析构。
3. 将 `DataNotifier::Instance()` 移到核心库并对该业务插件使用 `-fno-gnu-unique` 后，第 4 次
   运行进入真实 `dlclose`，却触发 SIGSEGV（CTest 退出码 8，单项失败率 1/1）。`gdb` 回溯定位到
   测试局部 `shared_ptr<ControlCommand>` 的最后一次释放：其控制块由插件内的
   `std::make_shared` 生成，虚析构入口已经随插件卸载。

### 根因、修复边界与回归

核心运行时必须唯一拥有跨 DSO 的 Dispatcher、Notifier 及业务输出消息控制块；业务插件只
借用这些运行时对象。`autodrive_runtime.h` 因而声明业务 Dispatcher 的显式实例和四类输出的
核心消息工厂，`minicyber_core` 提供定义；五个组件不再在插件内创建会越过卸载边界的控制块。
测试在 `Clear()` 前释放观察副本，符合“所有外部消息借用结束后才卸载插件”的所有权顺序。
`-fno-gnu-unique` 只施加于唯一业务插件，不改变核心库或 CyberRT 的 Component/Factory 职责。

最终以相同命令连续 5 轮通过（5/5），每轮均验证五个 TextFormat 配置、确定性链路、工厂注册
和 `Clear()` 后注销；`cmake --build build/debug -j2` 退出码 0，完整 CTest 为 46/46 通过，
`find /dev/shm -maxdepth 1 -name 'minicyber_*' -print` 无输出。可迁移经验是：可 `dlclose` 的
C++ 插件不能携带进程级 GNU unique 单例，也不能让核心缓存持有由插件生成的 `shared_ptr` 控制块。

## 一、使用原则

- 先确认问题位于协程、数据唤醒、调度、传输还是动态加载边界，再缩小到对应测试
  和进程。
- 优先使用可关闭的日志、`gdb`、`strace`、ASAN 和现有测试；不要在信号处理器、
  回调或锁保护区加入复杂临时代码来证明猜想。
- 回归必须覆盖正常路径和关闭路径。旧基准数值、已删除能力和未验证的性能结论都
  不能作为诊断依据。

阅读时先按故障边界定位：协程入口崩溃或泄漏看第二节，数据到达但任务不运行看第三节，
`/dev/shm` 残留看第四节，跨进程端点不可见看第五节，启动、关闭或插件卸载异常看第六节。
每条完整记录均按“复现条件与症状 -> 错误假设 -> 工具与定位路径 -> 根因 -> 修复边界 ->
回归命令 -> 通用经验”组织；历史条目中的真实退出码和失败率必须保留。

## 二、协程上下文与栈上引用

### x86_64 栈对齐和上下文切换

**复现条件与症状**：在 Linux x86_64 上创建独立栈，第一次切入普通 C++ 函数后再切回，
可出现首次入口或第二次恢复崩溃、参数寄存器错误，或者仅在优化构建和调用使用 SSE 的
函数时不稳定。该类问题发生在业务 Proc 之前，不能先归因于 Scheduler 或 Channel。

**错误假设**：曾容易把“栈顶地址是 16 字节倍数”当成函数入口已经满足 ABI，或认为只要
保存通用寄存器就无需验证 `%rdi` 和伪造返回地址。System V AMD64 ABI 要求普通函数入口
观察到 `rsp % 16 == 8`；`call` 本应压入的返回地址必须由手工栈布局等价表达。

**工具与定位路径**：用 `gdb` 在 `ctx_swap` 和协程入口检查 `$rsp`、`$rdi` 与返回地址，
再对照 `src/swap.S` 的寄存器槽位和 `src/croutine/detail/routine_context.cpp` 的初始 `sp`。
历史修复可用下列命令核对，不依赖事后口述：

```bash
git show 3c3b0b29a8234cf14a882c104148d61e3901ec9c -- \
  src/swap.S src/croutine/detail/routine_context.cpp tests/test_croutine.cpp
```

**根因与修复**：初始栈必须预留返回地址和寄存器区，使首次进入函数时满足 ABI；汇编同时
保存/恢复 `%rdi`，并补齐 ELF 函数类型、大小与非可执行栈元数据。测试
`RoutineContextTest.SwitchesBothDirectionsWithAbiAlignedEntry` 在入口直接断言
`rsp % 16 == 8`，并覆盖 phase 1 -> main -> phase 2 -> main，防止只验证单向切换。

**回归与通用经验**：

```bash
cmake --build build/debug -j2 --target test_croutine
ctest --test-dir build/debug --output-on-failure -R '^test_croutine$'
```

退出码为 0 才能证明当前 x86_64 路径未回退。手写上下文切换必须用 ABI 断言和双向切换
测试固定契约；当前实现不承诺 aarch64 或其他 ABI。

### MainFunc 栈冻结导致的 `shared_ptr` 泄漏

**复现条件与症状**：让协程回调结束后从 `MainFunc` 最后一次 `Yield`，且不再恢复该栈。
长期运行或 ASAN 泄漏检查会看到 `CRoutine` 与 128 KiB 栈仍被持有，即使任务状态已经是
`FINISHED`。单次功能测试若只看回调返回值，很容易漏掉该泄漏。

**错误假设**：这不是 `shared_ptr(this)` 创建第二控制块导致的 double free，也不是
Scheduler 映射忘记删除。`GetThis()` 返回的合法共享所有权本身没有错；错误在于最后一份
局部 `shared_ptr` 被留在永远不再展开的协程栈帧上。

**工具与定位路径**：先用 ASAN 或析构计数确认对象未释放，再沿 `CRoutine::MainFunc ->
GetThis -> Yield -> SwapContext` 检查最后一次切换前仍存活的栈变量。历史完整推导可通过
`git show 7d0aafe:docs/croutine/shared_from_this.md` 读取，当前实现位于
`src/croutine.cpp`。

**根因与修复**：协程栈在最后一次 `Yield` 后冻结，C++ 不会替它执行局部变量析构。
`MainFunc` 因而先保存 `cur`、清空 `cb_`、提取裸指针，再显式 `cur.reset()`，最后通过裸
指针 `Yield`；`Yield` 本身也不能重新生成跨切换存活的共享所有权。

**回归命令与通用经验**：

```bash
cmake --build build/debug -j2 --target test_croutine
ctest --test-dir build/debug --output-on-failure -R '^test_croutine$'
```

协程、fiber 或手工栈在“最后一次挂起”前必须主动释放需要析构的所有权对象。代码评审时
要区分“双控制块”与“冻结栈引用不下降”两类完全不同的 `shared_ptr` 问题。

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

**复现条件与症状**：一个线程进入 Notify 并开始执行回调，另一线程同时销毁 Reader、
Visitor 或 Component 并注销通知。没有生命周期屏障时会在关闭返回后继续回调已释放对象，
表现为 UAF；若注销无条件等待活动回调，回调内部自注销又会自死锁。

**错误假设**：仅用 map 互斥锁保护查找和删除并不够。若持锁执行用户回调会阻塞重入；若
锁外保存裸指针，删除后又会悬空；若所有注销都等待 `active_callbacks_ == 0`，当前回调会
等待自己结束。

**工具与定位路径**：以条件变量人为阻塞回调，稳定构造“回调在途、另一线程注销”，再用
线程栈确认等待点。检查 `DataNotifier::Notify` 是否取得 `shared_ptr` 快照、回调是否在表锁外
执行、`RemoveNotifier` 是否先摘路由后 Deactivate。历史改动：

```bash
git show 3cea6f5 -- include/minicyber/data/data_notifier.h tests/test_data_notifier.cpp
```

**根因与修复**：Notifier 对象需要独立于注册表项存活。Notify 在短锁内取得共享快照，锁外
递增在途计数并执行；Remove 先从表中摘除，清空 callback，再等待其他线程的在途回调归零。
thread-local `active_notifier_` 识别回调内自注销，使它不等待自己。

**回归命令与通用经验**：

```bash
cmake --build build/debug -j2 --target test_data_notifier
ctest --test-dir build/debug --output-on-failure -R '^test_data_notifier$'
```

必须保留 `RemoveNotifierWaitsForInFlightCallback` 与
`CallbackCanRemoveItselfWithoutDeadlock`。可注销回调的完成定义是“以后不再开始，其他
线程已开始的也已结束”，同时必须明确处理回调内自注销。

### AllLatest 填充必须先于唤醒

**复现条件与症状**：次通道已有最新值，主通道到达后协程进入 READY，却在 TryFetch 中
看不到融合结果，直到下一条主消息才处理上一轮数据。另一种症状是首条测量消息被最新值
语义跳过，后续连续流量掩盖了问题。

**错误假设**：AllLatest 不是两个通道任意到达都唤醒，也不是严格时间同步；增加锁或让
Visitor 再推进一次游标会制造重复消费。三、四输入模板已删除，不能拿历史泛型能力解释
当前双输入行为。

**工具与定位路径**：沿 `DataDispatcher::Dispatch -> CacheBuffer::Fill ->
DataNotifier::Notify -> Scheduler::NotifyTask -> DataVisitor::TryFetch` 记录主通道一次到达的
先后顺序；检查 `data_visitor.h` 的构造注册顺序和 `all_latest.h` 的 primary callback。

**根因与修复**：primary Fill 先执行融合 callback，读取 secondary 的 Latest 并写入独立融合
队列，Dispatcher 完成 Fill 后才 Notify；随后注册的 Scheduler notifier 才能唤醒协程。
销毁时清空 fusion callback 并等待在途执行，避免访问已释放的 secondary。首个游标的
最新值语义由 sequence 0 端到端预热解决，不修改 CacheBuffer 契约。

**回归命令与通用经验**：

```bash
cmake --build build/debug -j2 --target test_data_visitor test_routine_factory
ctest --test-dir build/debug --output-on-failure \
  -R '^test_(data_visitor|routine_factory)$'
```

融合系统必须明确“谁触发、取哪个快照、先产出还是先唤醒、谁推进游标”；仅验证连续高频
消息会漏掉首条唤醒和首游标问题。

## 四、POSIX SHM 异常清理

**复现条件与症状**：创建命名段后向创建者发送异常信号，或在 `Destroy()` 的登记注销与
`shm_unlink` 之间终止进程。`/dev/shm/minicyber_*` 残留会让下一轮按同名但不同布局打开时
失败。普通返回路径通过不能证明异常路径安全。

**错误假设**：进程退出只会回收映射和文件描述符，不会自动 unlink 名称；纯消费者也不能
代替创建者删除仍被其他进程使用的段。信号处理器里调用 C++ `Destroy`、日志、mutex、
`std::string` 或堆分配均不满足 async-signal-safe 约束。

**工具与定位路径**：测试前后用 `find /dev/shm` 比较精确名称，并沿
`PosixSegment::OpenOrCreate -> Register -> Destroy -> Unregister` 检查所有权。历史修复可用：

```bash
git show fd21302 -- src/transport/shm/posix_segment.cpp
git show 35079fb -- src/transport/shm/posix_segment.cpp
```

**根因与修复**：创建者在创建成功后把固定名称登记到无动态分配的信号清理表；异常路径只
调用 `shm_unlink`，恢复默认处理并重新抛出信号。正常 `Destroy()` 必须先确认 unlink 成功，
再从登记表移除；若先注销，恰在 unlink 前崩溃就会失去最后兜底。mainboard 的正常
SIGINT/SIGTERM 则在线程创建前阻塞并由主线程 `sigwait`，不依赖可能被 SHM 覆盖的 handler。

**回归命令与通用经验**：

```bash
cmake --build build/debug -j2 --target test_posix_segment_signal
ctest --test-dir build/debug --output-on-failure -R '^test_posix_segment_signal$'
```

运行前后执行 `find /dev/shm -maxdepth 1 -name 'minicyber_*' -print`，预期无本测试新增残留。
异常清理、正常优雅退出和多进程所有权是三条不同链路，不能用一个全局通配符清理脚本
掩盖生产路径的所有权错误。

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

### Scheduler 关闭与并发提交

**复现条件与症状**：一个线程执行 `Scheduler::Shutdown`，其他线程仍在 CreateTask 或
NotifyTask。旧行为可能把任务送入已停止 Context、在 Processor join 后访问被清空的映射，
或因 Wait 未被唤醒而关闭卡住；顺序创建两种策略还可能复用上一次静态状态。

**错误假设**：只设置一个原子 stop 标志不能覆盖“检查 stop 后、真正入表前”的窗口；只先
停止 Processor 也不能保证阻塞在 Context::Wait 的线程退出。该问题不是工作窃取，Classic
的正式语义始终是调度组共享多级优先级队列。

**工具与定位路径**：用提交线程循环 CreateTask/NotifyTask，同时由主线程 Shutdown，记录
是否有超时、重复执行或崩溃；再沿 Scheduler 生命周期锁、Context wait 条件和 Processor join
顺序检查。历史修复：

```bash
git show 3cb9e3a -- src/scheduler/scheduler.cpp \
  tests/test_scheduler_classic.cpp tests/test_scheduler_choreography.cpp
```

**根因与修复**：创建和通知在生命周期互斥区内二次检查 stop；Shutdown 先
`stop_.exchange(true)`，在锁内换出 Processor/Context，先 `context->Shutdown()` 唤醒 Wait，
再 `processor->Stop()` join，最后清任务映射。后续任务还让 `RoutineWakeState` 失效并处理
RemoveTask 在当前协程内关闭，避免回调触达已销毁 Scheduler 或等待自身 Acquire。

**回归命令与通用经验**：

```bash
cmake --build build/debug -j2 --target test_scheduler_classic test_scheduler_choreography
ctest --test-dir build/debug --output-on-failure \
  -R '^test_scheduler_(classic|choreography)$'
```

必须保留 `ConcurrentSubmissionDuringShutdown` 和 `PoliciesCanBeRecreatedInSequence`。
并发运行时的关闭协议需要同时定义拒绝新工作、唤醒等待者、join 执行线程、失效外部回调和
清理映射的顺序，原子布尔值不是完整生命周期协议。

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

### MC-617：ConditionNotifier 全局 SysV 队列与测试启动窗口

**触发环境与最小复现**：在 Debug 构建后执行
`ctest --test-dir build/debug --output-on-failure`。首次运行中
`ConditionNotifierTest.ListenTimeout` 在 3 ms 返回一条通知，预期的 100 ms 超时未发生；
CTest 退出码为 8，47 项中 1 项失败，当前观察失败率为 1/1。此前 MC-608 的
`ForkCrossProcessNotify` 曾有 1 次子进程退出码 1，随后复跑通过，不能以本次新现象覆盖。

**排查过程与证据**：先定向执行 `ListenTimeout` 20 轮，全部通过，说明没有稳定的
`Listen` 超时算法回归。随后读取 `ConditionNotifier::MakeKey/OpenOrCreate/Listen`，确认其与
`cyber_ref/cyber/transport/shm/condition_notifier.cc` 一样使用固定的跨进程 SysV key，而不是
测试进程私有队列；`ipcs -m` 也观察到同 key 的在途段。原 fork 用例还依赖 50 ms 固定 sleep，
无法区分子进程初始化、历史通知和实际通知传递。

**根因与修复边界**：根因是测试把固定 key 的共享通知环当作独占空队列，并以 sleep 编排
父子顺序。MC-617 不改变 ConditionNotifier 的原生共享职责：超时用例在开始计时前以
`Listen(0)` 排空当前可见通知，fork 正反向用例改以管道和 2 s 有界条件编排。该修复没有
删除跨进程通知、没有把轮询替换为 eventfd，也没有用延长 sleep 掩盖竞态。

**回归结果与经验**：修复后 `cmake --build build/debug -j2` 退出码 0，Debug 全量 CTest
为 45/45 通过。固定 key 的 IPC 测试必须把“已有可见通知”和“本用例发布通知”分开；只要
出现一次跨进程失败，仍需保留命令、退出码、失败率和后续归属，不能只报告最终通过。

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
主线程使用 `sigwait` 同步消费 pending 信号，不再安装会被 SHM 路径覆盖的正常退出 handler。测试插件在
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

### MC-616 多进程主干启动与尾部排空

**触发环境与最小复现**：Debug 构建中先后执行：

```bash
scripts/run_autodrive_pipeline.sh --messages 10 --frequency 20 --timeout-ms 15000 \
  --output-dir /tmp/minicyber-mc616-YTL44a
scripts/run_autodrive_pipeline.sh --messages 1000 --frequency 100 --timeout-ms 30000 \
  --output-dir /tmp/minicyber-mc616-default-X43Mdx
```

第一次命令的脚本退出码为 3，三进程均已启动但 Sink 未收到命令；第二次命令同样非零，
Sink 的退出码为 3。随后相同默认参数又分别得到 `978/1000` 和“Sink 指标完整但脚本退出
143”的结果；每次观察的失败率都是 1/1。这些跨进程/生命周期失败均保留在本条，不能以
后续复跑通过覆盖。

**原始现象与按时间排序的排查**：第一轮的 `mainboard.log` 记录：

```text
dlopen failed for: /home/liuxuanling/minicyber/libminicyber_autodrive_components.so
```

同时 Source 报“timed out waiting for CameraFrame and VehicleState readers”，Sink 报
“timed out after 0 of 10 commands”。检查 DAG 后确认 `module_library` 是不含目录的
`libminicyber_autodrive_components.so`，而启动脚本已将构建目录加入 `LD_LIBRARY_PATH`。
继续跟踪 `ModuleController::ResolveLibraryPath`，发现它仍把所有相对字符串拼成 CWD，
因而绕过 `dlopen` 的动态库搜索路径。将“裸库名”保留给 `dlopen`、仅让含目录的相对路径
拼 CWD 后，同一 10 条负载通过，Sink 为 `received=10 missing=0 out_of_order=0`。

第二轮在加载修复后复现 1000 条默认负载。`metrics.txt` 输出
`received=994 missing=6 out_of_order=0 throughput_mps=100.110`，Sink 日志为
“timed out after 994 of 1000 commands”。先让 Source 在 `--await-shutdown` 模式下保留
Node 和 Writer，直到 Sink 的完整序列确认后才由脚本 SIGTERM；这排除了“发完即撤销输入
Writer”作为唯一解释，但下一次仍得到 `received=978 missing=22 out_of_order=0`。因此不能用
扩大 SHM 块数或改变 `PosixSegment::Destroy` 的引用计数语义掩盖问题。

继续从 Channel Join 时序检查：Source 原先只等待 CameraFrame 与 VehicleState 的远端 Reader，
并不证明 ControlComponent 的 ControlCommand Writer 已经观察到先启动的 Sink Reader。Hybrid
对端关系是异步收敛的；Writer 在没有对端时按既有 API 返回发布成功，所以首段业务输入可在
ControlCommand 的 SHM 连接建立前静默越过 Sink。ControlSink 因此增加 `Reader::HasWriter` 的
有界等待，并写入只供启动脚本读取的 ready 文件；脚本在启动 Source 前轮询这个明确 Join 条件。
轮询具有同一 `--timeout-ms` 上界，不能以固定 sleep 猜测发现完成。Source 仍保留到 Sink
确认后的 Writer 排空，结束顺序固定为 Sink 成功 -> SIGTERM Source -> SIGTERM mainboard。

下游 Join 门禁首次得到完整 Sink 指标，但脚本退出 143，失败率 1/1，mainboard 日志没有
`ModuleController cleared`。单独在无后续 Join 的 mainboard 上发送 SIGTERM 可退出 0，说明
不是组件关闭本身。进一步确认 Control Writer 在 DAG 加载返回后的异步 Reader Join 中创建
POSIX SHM 段，其异常清理处理器覆盖了 mainboard 先前的异步 SIGTERM handler；此时正常退出被
重新抛出。mainboard 改为在创建 Scheduler 线程前阻塞 SIGINT/SIGTERM，并由主线程 `sigwait`
同步消费。这个消费路径不依赖会被 SHM 异常处理器覆盖的 handler，既覆盖初始化期间已挂起的
信号，也覆盖阻塞等待边界；SIGSEGV 仍由 SHM 异常清理路径处理。

trap 在全部 PID 已回收后，只通过 `Transport::ChannelNameToId` 计算四个唯一主干频道并精确
`shm_unlink`，忽略 `ENOENT`，不使用 `/dev/shm/minicyber_*` 通配符，且不改变
`PosixSegment` 的正常引用计数职责。

修复后执行：

```bash
scripts/run_autodrive_pipeline.sh --messages 1000 --frequency 100 --timeout-ms 30000 \
  --output-dir /tmp/minicyber-mc616-sigwait-9rqkvo
find /dev/shm -maxdepth 1 -name 'minicyber_*' -print
```

脚本退出码为 0，指标为 `received=1000 missing=0 out_of_order=0`、
`throughput_mps=100.109`、`latency_ns_p50=1838319`、`latency_ns_p95=4558085`、
`latency_ns_p99=6165685`；mainboard 记录 `ModuleController cleared`，第二条命令无输出。
后续归属：MC-618/MC-619 分别以相同入口完成
Classic/Choreography 的正式集成验收，MC-620 才能将该入口的 Release 数据写成性能结论。

### MC-616 质量评估：短队列覆盖与验收证据失真

**触发环境与现象**：评估首次执行 20 条/100 Hz 时 Sink 只收到 14 条，
退出码为 3；同参数后续连续 3 次通过。增加 ControlAudit 集合观察后，一次默认
1000 条运行同时得到 `control=993` 与 `audit=993`，两者差集为 0。这排除了
Audit 二次发布或 Sink Hybrid 扇出单独丢失的候选假设。

**排查顺序与工具**：先用唯一脚本分别重复小负载和默认负载，保留每轮进程日志、
退出码和 metrics；再用 `rg`/`sed` 沿 `ShmDispatcher -> DataDispatcher -> DataVisitor ->
ChannelBuffer -> RoutineFactory` 追踪数据窗口。`ConditionNotifier` 环容量为 4096，本次负载
未超过通知环；而唯一 DAG 中五个 Reader 的 `pending_queue_size` 均为 8。
`ChannelBuffer::Fetch` 明确规定消费游标落后于 `Head` 时快进到 `Tail`，因而 Processor
短时调度抖动会合法覆盖中间业务消息。

**根因和修正边界**：队列深度 8 会放大落后覆盖风险，但调整为 1024 后仍实测到
`999/1000`，因而“只是队列太短”的假设被排除。继续检查 `ChannelBuffer::Fetch`，确认
消费游标初值为 0 时会直接读取当前 `Tail`；如果某层 Processor 首次执行前已累积
两条消息，第一条会按原生“首次取最新”语义跳过，队列再大也无法修复。
最终不修改 CacheBuffer/ChannelBuffer 语义；SensorSource 同时发布 sequence 0 的 VehicleState
和 CameraFrame，并等待这条预热真正穿过五个 Component、由 ControlAudit 经 SHM 回到
Source 后，才发布 1..N 测量序列。这是可观测的端到端预热屏障，不以 sleep 猜测
协程是否已消费首条数据。DAG 业务队列仍设为 1024，用于覆盖默认验收窗口中
预热之后的短时调度抖动。

**附带证据修正**：旧脚本每次退出都无条件 `shm_unlink`，因而“无残留”不能证明
引用归零的自然回收。新路径在成功时先检查四个精确 POSIX SHM 名称，仅在失败或
中断后才做精确兜底删除。这使“自然回收”和“环境恢复”成为两类可区分证据。

### MC-618 Classic 主干 CTest 工作目录与 SHM 块覆盖

**触发环境与最小复现**：Debug 构建先后执行：

```bash
ctest --test-dir build/debug --output-on-failure -R '^test_autodrive_classic_pipeline$'
ctest --test-dir build/debug --output-on-failure -L high_risk
```

首次 CTest 退出码为 8、1 项中 1 项失败。`mainboard.log` 已加载唯一 DAG 与
`libminicyber_autodrive_components.so`，但在 `PerceptionComponent::Initialize` 失败，
Sink 在 30 秒后报未观察到 ControlCommand Writer，Source 尚未启动。该次失败率为 1/1；
四个精确频道的 `build/debug/bin/control_sink --check-shm-clean` 退出码为 0。第二次同命令
在修正工作目录后业务主干已完整输出 `received=1000`、`audit_received=1000`，但 CTest 仍以
退出码 8、1/1 失败：正常成功的 `control_sink.log` 为 0 bytes，而驱动错误地将每个日志都
断言为非空；SHM 检查仍为 0。两次均保留在
`build/debug/autodrive-classic-ctest-*` 的独立进程日志目录，不能由后续通过删除。

**真实数据失败与排查**：修正 CTest 驱动后，单独主干通过；随后完整
`ctest --test-dir build/debug --output-on-failure -L high_risk` 退出码为 8，22 项中 21 项通过。
本轮 `control_sink.log` 为：

```text
ControlSink timed out after 994 control and 998 audit of 1000 commands.
```

对应指标为 `received=996 audit_received=1000 lost=6 duplicates=2 audit_duplicates=2
out_of_order=2 audit_difference=4`；mainboard 已动态加载全部五个 Component，Source 已报告
发布完成，且 SHM 检查退出码为 0、无残留进程或精确频道残留。因此排除发现未 Join、异常
退出和尾部提前销毁，失败率为 1/1。

沿 `ShmTransmitter -> PosixSegment -> ConditionNotifier -> ShmDispatcher` 检查后确认，正式
Protobuf SHM 原先统一使用 `64 KiB/4 blocks`。写锁在提交后立即释放，Dispatcher 尚未读取的
块可被四次循环后的新写覆盖；若同一 `HybridTransmitter` 同时向本地 Audit INTRA 和远端 Sink
SHM 扇出，INTRA 成功会使 `Transmit` 返回成功，掩盖远端块已覆盖。通知仍指向旧块索引，因而
Sink 可观察到重复、乱序与尾部缺失。这不是扩大 Component 队列或改变 CacheBuffer 快进语义
能够修复的问题。

**修复边界与回归**：参照 `cyber_ref/cyber/transport/shm/shm_conf.cc` 的首档，正式
Reader/Writer 默认改为共同请求 `16 KiB/512 blocks`；`PosixSegment` 的显式低层测试默认值和
MiniCyber 未实现的动态分档/重建范围均不改变。新增 `test_hybrid_transport` 定向断言锁定首档，
并新增 `test_autodrive_classic_pipeline`，其只调用唯一脚本、保存三进程日志、检查真实 dlopen
的五组件、1000 条 Control/Audit 指标与自然 SHM 回收。

```bash
cmake --build build/debug -j2
ctest --test-dir build/debug --output-on-failure --repeat until-fail:3 -R '^test_autodrive_classic_pipeline$'
ctest --test-dir build/debug --output-on-failure -L high_risk
ctest --test-dir build/debug --output-on-failure
```

实际退出码均为 0；Classic CTest 连续 3/3 通过，high_risk 为 22/22，完整 Debug CTest 为
46/46。每轮成功路径均在脚本兜底 `shm_unlink` 前通过四频道检查。后续 MC-619 只复用该入口
验证 Choreography 与混合扇出，不得复制业务管道或回退 SHM 首档容量。

### MC-620 质量评估：功能取证污染性能路径

**触发条件与现象**：检查 MC-620 原始 JSON 时发现提交号仍指向 MC-619，工作区状态为
`dirty`。继续沿采集脚本进入统一启动脚本，确认每次运行都无条件设置
`MINICYBER_AUTODRIVE_EVIDENCE_FILE` 并向 Sink 传入 `--evidence-file`。这意味着五个组件
每条消息都会执行环境变量查询、全局互斥和 `map/set` 插入，Control/Audit 还记录指针与
后端计数；这些操作属于 MC-619 功能验收，不属于端到端延迟指标。

**排查与排除**：先对照 `demo/autodrive/evidence.cpp` 确认 `Enabled()` 只受环境变量控制，
再对照 `scripts/run_autodrive_pipeline.sh` 确认性能入口确实总是设置该变量。metrics 自身的
开关测试只能证明关闭 metrics 后不保存延迟样本，不能排除另一套 evidence 的逐消息开销，
因此“已有 metrics 开关足以隔离测量开销”的假设不成立。

**修正边界**：统一脚本增加独立 `--evidence/--no-evidence`，默认关闭；Choreography 功能
验收显式开启，性能采集显式关闭。既有 CSV/JSON 不删除，改标为临时观测；最终数据由
MC-623 在代码、脚本和配置均已提交且工作区干净的 Release 基线上重采。这样没有修改
Transport、Scheduler 或业务算法，只隔离验证探针与性能样本。

## 七、证据来源

本文事实迁移自首轮 `00_进度记录.md`、`baseline.md`、`module_mapping.md`、
`croutine/shared_from_this.md`、`scheduler/debug_vtable_hang.md`、
`transport/signal.md` 及相关测试，并由 MC-622 对照现存源码、提交和测试定稿。旧文件已在
MC-602 删除；后续新增重大故障仍须按九项排错链增量更新，不能只补根因和修复结论。
