# 信号处理防SHM泄漏技术方案背景
该方案是针对POSIX共享内存的原生特性、进程异常终止场景下的资源泄漏痛点，结合异步信号编程约束设计的底层可靠性兜底机制，核心背景可分为五层：

## 一、底层机制根源：POSIX共享内存的内核持久化特性
POSIX 命名共享内存是**内核级持久化资源**，这是所有问题的底层根源：
1.  它以 tmpfs 文件形式存在于系统 `/dev/shm` 目录，生命周期与创建进程完全解耦；
2.  进程退出时，操作系统只会自动回收进程内的虚拟地址映射、文件描述符，**不会自动删除共享内存文件本身**；
3.  只有主动调用 `shm_unlink` 删除名称，且所有进程都解除内存映射后，内核才会真正释放物理内存，否则它会一直存在直到系统重启。

这一特性是共享内存实现高性能多进程通信的基础，但也天然带来了「进程退出、资源残留」的本质风险。

## 二、核心问题：异常终止场景下的资源泄漏
`PosixSegment` 正常采用 RAII 设计：对象析构时自动调用 `Destroy()`，内部执行 `shm_unlink` 完成清理，正常流程下不会泄漏。

但当进程遭遇**非正常终止**时，C++ 对象析构逻辑不会执行，正常回收机制完全失效，共享内存文件会永久残留在系统中。典型场景包括：
- 人工停止：收到 `SIGINT`（Ctrl+C 中断）、`SIGTERM`（服务停止信号）等强制终止信号；
- 程序崩溃：触发 `SIGSEGV`（段错误）、`SIGABRT`（断言失败）等致命异常；
- 其他致命信号导致的进程异常退出。

## 三、泄漏的实际危害
共享内存残留不是单纯的内存浪费，会对基础通信组件的可靠性、运维效率、稳定性造成直接影响：
1.  **系统资源耗尽**：大量残留共享内存会持续占用 `/dev/shm` 空间，最终导致系统共享内存不足，新服务无法创建通信通道、启动失败；
2.  **服务重启异常**：同名称共享内存残留后，服务重启时会复用旧的共享内存段，若块数量、单块大小等参数不匹配，会导致通信逻辑异常、数据错乱；
3.  **运维成本升高**：需要人工定期清理残留文件、排查服务启动失败问题，长期运行的线上节点问题更突出；
4.  **测试环境不稳定**：单元测试、集成测试中进程异常退出后，残留共享内存会导致后续用例数据污染、随机失败。

## 四、方案设计的硬性约束：异步信号安全
最直观的思路「在信号处理器中直接调用 `Destroy()` 清理资源」完全不可行，因为存在**异步信号安全**的编程强约束：
信号处理器是异步打断进程正常执行流的回调，相当于在任意代码行中间插入执行。此时调用非信号安全的函数，可能触发死锁、内存损坏、未定义行为等更严重的问题。

POSIX 标准明确规定了信号处理器中仅允许调用有限的 `async-signal-safe` 函数: 在信号处理函数中调用，不会因为异步重入导致死锁、状态损坏、未定义行为的函数。而 `PosixSegment` 的销毁逻辑包含大量非安全操作：
- `std::string` 成员底层涉及动态内存分配（`malloc/free`），不属于信号安全函数；
- 注册/注销逻辑中的 `std::mutex` 互斥锁，信号中加锁可能打断正在持锁的线程，造成永久死锁；
- 对象析构、成员访问等操作，在信号上下文中存在状态不一致的风险。

因此不能直接复用类的销毁逻辑，必须设计一套**独立于对象生命周期、完全满足异步信号安全**的兜底清理机制，这也是「全局静态数组 + 原子计数 + 信号只读」注册表设计的核心原因。

## 五、设计目标与边界
基于上述背景，该方案设定了明确的设计边界：
1.  仅兜底清理**本进程创建**的共享内存：消费者进程崩溃不执行清理，避免影响其他正在通信的进程；
2.  严格遵循异步信号安全规范，信号处理路径不使用任何非安全函数、不加锁、不分配内存；
3.  对上层业务透明，仅在底层自动生效，不增加接口使用成本；
4.  清理完成后恢复信号默认行为，保证进程退出码、core dump 等原生表现不受影响。


---
下面我们顺着源码从上到下逐段拆解，每一处设计都对应前面提到的「POSIX共享体内核持久化风险」「异步信号安全强约束」两大核心背景。

## 一、匿名命名空间：内部核心基础设施
代码最外层用了匿名 `namespace`，把注册表、信号处理等内部实现完全隐藏，只通过 `PosixSegment` 的成员函数对外暴露，避免污染全局命名空间。

### 1. 常量与注册表结构体
```cpp
constexpr size_t kMaxShmNames = 64;
constexpr size_t kMaxNameLen = 64;

struct ShmNameRegistry {
  char names[kMaxShmNames][kMaxNameLen];
  std::atomic<int> count{0};
  std::mutex mutex;
};
```
这是整个方案的核心数据结构，每一项设计都服务于「异步信号安全」：
- **固定长度二维字符数组 `names`**：不用 `std::string`/`std::vector`，因为它们底层依赖 `malloc/free` 动态内存分配，属于非信号安全操作。固定长度数组是纯静态内存，信号中可以安全读取。64个通道、单名称64字节的容量，完全覆盖业务场景。
- **原子计数 `count`**：表示当前数组中有效名称的数量。
  - 信号处理器只读取这个值，无需加锁，避免信号上下文死锁；
  - 写入时严格遵循「先写名字数据，再更新count」的顺序，保证信号读到的count对应的名称一定是完整合法的。
- **互斥锁 `mutex`**：仅用于正常流程下多线程注册/注销的线程安全。**信号处理路径绝对不会碰这把锁**，这是避免死锁的关键。

### 2. 全局注册表单例
```cpp
ShmNameRegistry& Registry() {
  static ShmNameRegistry r;
  return r;
}
```
采用 Meyers 单例模式：
- 整个进程只有一份全局注册表，所有 `PosixSegment` 实例共享；
- C++11 保证局部静态变量初始化是线程安全的，无需手动加锁。

### 3. 信号处理器 CrashHandler
```cpp
void CrashHandler(int sig) {
  ShmNameRegistry& r = Registry();
  int n = r.count.load(std::memory_order_relaxed);
  for (int i = 0; i < n; ++i) {
    ::shm_unlink(r.names[i]);
  }
  ::signal(sig, SIG_DFL);
  ::raise(sig);
}
```
这是崩溃时的兜底清理逻辑，全程严格遵守异步信号安全：
1.  读取原子计数 `count`，使用 `memory_order_relaxed` 最弱内存序。
    - 信号是在触发信号的当前线程内执行的，本线程之前的写入天然可见，不需要重内存屏障，性能最优。
2.  遍历所有已注册名称，逐个调用 `shm_unlink` 删除共享内存文件。
    - `shm_unlink` 是 POSIX 标准明确标记的 async-signal-safe 函数，是整个清理动作的核心。
3.  恢复信号默认处置，再主动重新抛出信号。
    - 不直接 `exit()`：保留进程原生退出行为，保证退出码正确、能正常生成 core dump，父进程可以感知真实崩溃原因，仅在退出前多做一步 shm 清理。

### 4. 单次安装逻辑 InstallOnce
```cpp
bool InstallOnce() {
  if (HandlerInstalled().load(std::memory_order_acquire)) return false;
  static std::once_flag once;
  bool first = false;
  std::call_once(once, [&] {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = CrashHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGSEGV, &sa, nullptr);
    HandlerInstalled().store(true, std::memory_order_release);
    first = true;
  });
  return first;
}
```
保证整个进程生命周期内，信号处理器只安装一次：
- **双重检查优化**：先用原子变量快速判断是否已安装，避免每次都走 `std::call_once` 的开销；`acquire` 内存序保证读到 `true` 时，之前的安装操作一定可见。
- **`std::call_once` 保证唯一执行**：多线程并发调用也只会执行一次安装逻辑，线程安全。
- **`sigaction` 替代 `signal`**：更标准、跨平台行为一致，设置 `SA_RESTART` 标志，让被信号打断的慢速系统调用自动重启，减少业务侧出现 `EINTR` 错误的概率。
- **监听三个致命信号**：
  - `SIGINT`：Ctrl+C 人工中断；
  - `SIGTERM`：系统/容器停止服务的标准信号；
  - `SIGSEGV`：段错误程序崩溃。

## 二、对外接口层：注册、注销与工具函数
这一层是 `PosixSegment` 类的成员函数，封装了对注册表的操作，运行在正常业务上下文，允许加锁、使用 `std::string` 等非信号安全组件。

### 1. 注册共享内存名称
```cpp
void PosixSegment::RegisterShmName(const std::string& name) {
  ShmNameRegistry& r = Registry();
  std::lock_guard<std::mutex> lg(r.mutex);
  int n = r.count.load(std::memory_order_relaxed);
  // 去重
  for (int i = 0; i < n; ++i) {
    if (std::strncmp(r.names[i], name.c_str(), kMaxNameLen) == 0) return;
  }
  // 容量检查
  if (n >= static_cast<int>(kMaxShmNames)) return;
  // 写入名称
  std::strncpy(r.names[n], name.c_str(), kMaxNameLen - 1);
  r.names[n][kMaxNameLen - 1] = '\0';
  // 最后更新计数
  r.count.store(n + 1, std::memory_order_release);
}
```
执行流程与设计要点：
1.  加互斥锁，保证多线程并发注册时数据安全；
2.  遍历去重，避免同一个通道重复注册；
3.  容量超限则直接返回，保证数组不越界；
4.  用 `strncpy` 拷贝名称，手动补 `\0`，保证 C 字符串合法，信号中读取不会越界；
5.  **先写数据，最后更新计数**，并用 `release` 内存序：
    - 保证所有 CPU 看到的顺序都是「名字写完 → count增加」；
    - 信号处理器读到 count 时，对应的名称一定已经完整写入，不会读到半截字符串。

### 2. 注销共享内存名称
```cpp
void PosixSegment::UnregisterShmName(const std::string& name) {
  ShmNameRegistry& r = Registry();
  std::lock_guard<std::mutex> lg(r.mutex);
  int n = r.count.load(std::memory_order_relaxed);
  for (int i = 0; i < n; ++i) {
    if (std::strncmp(r.names[i], name.c_str(), kMaxNameLen) == 0) {
      // 用最后一个元素覆盖当前位置
      std::memmove(r.names[i], r.names[n - 1], kMaxNameLen);
      r.count.store(n - 1, std::memory_order_release);
      return;
    }
  }
}
```
删除逻辑的设计细节：
- 采用「尾元素覆盖法」：找到待删除项后，用数组最后一个有效元素覆盖它，再把计数减一。
  - 好处是 O(1) 时间复杂度，不需要移动大量元素；
  - 注册表不需要保序，只要记录所有有效名称即可，顺序无关紧要。
- 使用 `memmove` 而非 `memcpy`：处理首尾位置重叠的边界场景，保证拷贝正确性。

### 3. 调试与测试工具函数
- `InstallSignalHandler()`：对外暴露的信号安装入口，内部调用 `InstallOnce()`。
- `RegisteredShmNames()`：加锁后把所有名称转成 `std::vector<std::string>` 返回，用于调试、测试断言。
- `ClearRegistryForTest()`：测试专用，清空注册表计数，实现用例间环境隔离。
- `CleanupAllForTest()`：测试专用，遍历注册表删除所有 shm 并清空计数，用于测试收尾统一清理。

> 注意：这些函数都运行在正常流程，所以可以放心使用 `std::vector`、`std::string`，不会触发信号安全问题。

## 三、生命周期挂钩：与对象创建/销毁绑定
注册表和信号处理器不是独立运行的，它和 `PosixSegment` 的生命周期深度绑定，只有创建者进程会注册，正常销毁时会注销。

### 1. 创建时注册：OpenOrCreate
```cpp
// ... 共享内存创建、初始化成功后 ...
state_->IncreaseReferenceCounts();
InstallSignalHandler();
RegisterShmName(shm_name_);
opened_ = true;
return true;
```
两个关键设计决策：
1.  **只有创建者会注册**：走 `OpenOrCreate` 新建共享内存的进程才会执行注册和信号安装；走 `OpenOnly` 纯打开的消费者进程不会注册。
    - 原因：创建者是共享内存生命周期的负责人，消费者崩溃不应该删除共享内存，否则会导致其他正常工作的进程通信中断。
2.  **初始化成功后再注册**：只有共享内存创建、映射、构造全部成功，才会加入注册表。如果创建中途失败，回滚逻辑里已经 `shm_unlink` 了，不需要再注册。

### 2. 销毁时注销：Destroy
```cpp
void PosixSegment::Destroy() {
  Close();
  if (!shm_name_.empty()) {
    UnregisterShmName(shm_name_);
    shm_unlink(shm_name_.c_str());
  }
}
```
执行顺序：先解除映射 → 再删除共享内存文件 → 最后从注册表注销。
- 崩溃安全：只要走到shm_unlink，共享内存一定被删除；
- 就算 unlink 后崩溃，shm_unlink 重复调用无害：文件不存在时仅返回 - 1、errno=ENOENT，无崩溃、无副作用，少量冗余调用完全可接受；

## 四、完整流程串联
### 正常生命周期
1.  创建者进程调用 `OpenOrCreate()`，共享内存创建成功；
2.  首次创建时安装全局信号处理器，后续创建直接跳过；
3.  将当前 shm 名称加入全局注册表；
4.  业务运行，正常调用 `Destroy()` / 对象析构；
5.  先解除映射，再 `shm_unlink` 删除文件，最后从注册表注销名称。

### 异常崩溃生命周期
1.  进程运行中收到 `SIGINT`/`SIGTERM`/`SIGSEGV` 致命信号；
2.  中断当前执行流，进入 `CrashHandler`；
3.  无锁读取注册表，遍历所有本进程创建的 shm 名称，逐个执行 `shm_unlink`；
4.  恢复信号默认处理，重新抛出信号；
5.  进程按原生方式退出，共享内存文件已被清理，不会残留泄漏。