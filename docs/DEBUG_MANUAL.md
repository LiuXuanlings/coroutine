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

## 五、调度启动、关闭和动态库

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

## 六、证据来源

本初稿迁移自首轮 `00_进度记录.md`、`baseline.md`、`module_mapping.md`、
`croutine/shared_from_this.md`、`scheduler/debug_vtable_hang.md`、
`transport/signal.md` 及相关测试。旧文件已在 MC-602 删除；后续任务必须以现存
源码、02/03 定稿和新的验证结果更新本文。
