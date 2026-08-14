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

## 七、证据来源

本初稿迁移自首轮 `00_进度记录.md`、`baseline.md`、`module_mapping.md`、
`croutine/shared_from_this.md`、`scheduler/debug_vtable_hang.md`、
`transport/signal.md` 及相关测试。旧文件已在 MC-602 删除；后续任务必须以现存
源码、02/03 定稿和新的验证结果更新本文。
