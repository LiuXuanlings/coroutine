This is the master plan for refactoring Sylar into MiniCyber. When you lose context, read this file to know where we are.

### Phase 1：调度引擎核聚变与无锁基建 (约 600 行)
**目标**：重构基础调度设施，消除原生协程库的全局锁瓶颈，引入 CPU 亲和性与工作窃取，为后续的“数据驱动”铺平道路。

**要写的文件**：
*   `base/macros.h`, `base/wait_strategy.h`, `base/bounded_queue.h`, `base/atomic_rw_lock.h`
*   `croutine/croutine.h / .cpp`
*   `scheduler/common/pin_thread.h / .cpp`
*   `scheduler/processor.h / .cpp`, `scheduler/policy/classic_context.h / .cpp`
*   `scheduler/scheduler.h / .cpp`

**核心改动**：
1. **状态机拓展**：将协程状态机增加 `DATA_WAIT`，支持主动让出执行权。
2. **多级调度队列**：废弃单把锁保护的 `std::queue`，引入缓存行对齐的无锁环形队列 `BoundedQueue` 作为全局兜底。
3. **工作窃取与绑核**：为每个工作线程（Processor）分配本地队列与优先级策略（Classic），并在空闲时通过 `Steal()` 从其他线程尾部无锁窃取任务。

**Phase 1 产出**：一个具备高缓存命中率、支持 N:M 负载均衡、且支持协程特定状态挂起的纯净版底层调度器。
**对应简历句**：基于 `epoll` 设计 N:M 协程调度器，针对全局队列锁竞争问题，引入 `BoundedQueue` 配合 `CacheLine` 内存对齐消除伪共享；实现基于**工作窃取（Work-Stealing）**与 CPU 亲和性绑核的多优先级调度策略，大幅提升多核利用率。

---

### Phase 2：数据驱动中枢 (约 800 行)
**目标**：赋予协程“因数据而生，因数据而眠”的能力。突破原生协程“只能等网络/定时器”的局限。

**要写的文件**：
*   `data/cache_buffer.h`
*   `data/channel_buffer.h`
*   `data/data_notifier.h / .cpp`
*   `data/data_dispatcher.h / .cpp`
*   `data/data_visitor.h`
*   `data/data_fusion.h` (高光点)

**核心改动**：
1. **零拷贝通道缓冲**：实现 `CacheBuffer` 环形覆盖缓冲，维持最新业务数据。
2. **异步唤醒链路**：实现 `DataDispatcher` 和 `DataNotifier`。当上游写入数据时，中枢自动找到处于 `DATA_WAIT` 的协程，将其状态改为 `READY` 并重新抛入 Scheduler。
3. **屏障同步 (Barrier)**：实现 `DataFusion`，提供 `WaitForAllLatest`，让一个协程同时等待多个 Channel 的数据（如雷达+视觉），全齐了才唤醒。

**Phase 2 产出**：一个进程内的零拷贝发布-订阅数据总线，协程直接挂载在通道上。
**对应简历句**：突破传统协程仅依赖系统 IO 唤醒的局限，设计 `DataDispatcher` 数据流转中枢。实现协程状态机（`DATA_WAIT` -> `READY`），协程因数据未就绪主动 Yield，数据到达后无锁异步唤醒；开发 `DataFusion` 模块，支持多通道数据的时间戳对齐与多协程屏障同步。

---

### Phase 3：零拷贝 IPC 与跨进程网络 (约 800 行)
**目标**：打破进程壁垒，将独立的计算进程融合成一张网，并利用 `eventfd` 将跨进程通信纳入本进程的协程调度系统。

**要写的文件**：
*   `transport/shm/state.h`, `block.h`, `segment.h`
*   `transport/shm/posix_segment.h / .cpp`
*   `transport/shm/condition_notifier.h / .cpp` (eventfd封装)
*   `transport/dispatcher/intra_dispatcher.h / .cpp`
*   `transport/dispatcher/shm_dispatcher.h / .cpp`
*   `transport/transmitter/...`, `transport/receiver/...`

**核心改动**：
1. **物理内存映射**：通过 `shm_open` + `mmap` 分配共享内存（Segment），并处理 SIGINT 信号防止 `/dev/shm` 泄漏。
2. **跨进程唤醒桥接**：**这是最精妙的一步**。使用 `eventfd` 传递跨进程到达通知，并**将该 `eventfd` 注册进底层的 `epoll` 树中**。当数据写入共享内存时，触发 `eventfd`，本进程的 epoll_wait 醒来，读取 SHM 并直接唤醒关联协程。
3. **混合收发器**：封装 `IntraTransmitter` (同进程) 和 `ShmTransmitter` (跨进程)。

**Phase 3 产出**：具备跨进程零拷贝能力的底层传输链路，协程跨进程唤醒延迟压进微秒级。
**对应简历句**：针对跨进程通信瓶颈，实现基于 `shm_open + mmap` 的共享内存映射；抛弃传统 IPC 锁，采用**原子序号（Indicator）+ `eventfd` 跨进程唤醒**方案；并将 `eventfd` 优雅融入底层的 `epoll` 监听树，实现跨进程消息的微秒级协程调度。

---

### Phase 4：组件模型与动态拓扑发现 (约 500 行)
**目标**：收口底层复杂性，向开发者提供极致简洁的 API，并自动决策数据包该走内存还是走共享内存。

**要写的文件**：
*   `topology/topology_manager.h / .cpp`
*   `transport/transport.h / .cpp`
*   `node/node.h / .cpp`
*   `node/reader.h`, `node/writer.h`

**核心改动**：
1. **有向图拓扑**：实现 `TopologyManager`，维护节点间的发布/订阅图。
2. **自动路由**：实现 `Transport::CreateTransmitter`，根据拓扑中 `IsSameProc` 的判断结果，运行时动态决定实例化 `Intra` 还是 `Shm` 通信通道。
3. **用户 API**：提供极简的 `node->CreateReader("/chatter", callback)` 语义。

**Phase 4 产出**：一套高内聚、易扩展的开发者 SDK 接口。
**对应简历句**：设计面向计算图的组件模型（`Node/Reader/Writer`），实现基于有向图（Graph）的本地拓扑管理；并在节点间建立通信时，依赖拓扑关系自动进行路由决策（INTRA 零拷贝 / SHM 共享内存），对业务层完全透明。

---

### Phase 5：极限无锁化优化与基准测试 (约 300 行)
**目标**：消除系统中最后一把读写锁，用硬核数据证明框架实力。

**要写的文件**：
*   `base/atomic_hash_map.h / .cpp`
*   `time/time.h`, `time/duration.h`
*   `examples/talker.cpp`, `examples/listener.cpp`
*   `examples/benchmark_pingpong.cpp`

**核心改动**：
1. **极限优化**：使用基于 CAS 链表解决冲突的 `AtomicHashMap` 替换 `DataDispatcher` 中的 `std::shared_mutex`，完成数据分发链路的全无锁化。
2. **基准测试**：提供 Time 库，编写 Ping-Pong 测试对比 `pipe+thread` 与 `Shm+Coroutine` 的吞吐和延迟差距。

**Phase 5 产出**：工业级优化收尾及可展示给面试官的 Benchmark 数据。

---

## 最终文件结构

```text
minicyber/
├── CMakeLists.txt
├── README.md
├── include/minicyber/
│   ├── base/               # BoundedQueue, AtomicHashMap, WaitStrategy, AtomicRWLock
│   ├── croutine/           # CRoutine, RoutineState
│   ├── scheduler/          # Scheduler, Processor, ClassicContext, PinThread
│   ├── data/               # CacheBuffer, ChannelBuffer, DataDispatcher, DataNotifier, DataVisitor, DataFusion
│   ├── transport/          # Transport, Segment, ConditionNotifier, ShmDispatcher, IntraDispatcher, Transmitter, Receiver
│   ├── topology/           # TopologyManager, Graph
│   ├── node/               # Node, Reader, Writer
│   └── time/               # Time, Duration
├── src/
│   ├── croutine/           # croutine.cpp, swap.S (底层汇编)
│   ├── scheduler/          # scheduler.cpp, processor.cpp, classic_context.cpp, pin_thread.cpp
│   ├── transport/          # shm_dispatcher.cpp, posix_segment.cpp, condition_notifier.cpp
│   ├── topology/           # topology_manager.cpp
│   ├── node/               # node.cpp
│   └── time/               # time.cpp
├── tests/                  # 各种底层组件的单元测试 (test_bounded_queue, test_shm...)
└── examples/               # 核心展示区
    ├── talker.cpp          # 节点发送示例
    ├── listener.cpp        # 节点接收示例
    └── benchmark_pingpong.cpp # 压测程序
```

---

## 面试时的黄金讲述路线（强烈建议按此逻辑引导面试官）

在面试时被问到“介绍一下你的框架”时，请按这 5 层递进逻辑输出（**制造“技术代差”碾压**）：

1. **第一层：痛点起手（Scheduler）**
   > “传统的 C++ 协程库（如 libco）通常使用一把大锁保护的全局队列，在多核场景下锁竞争非常严重。因此我**首先重构了调度核心**，引入了缓存行对齐的无锁有界队列 `BoundedQueue` 作为兜底，并为每个核心（Processor）分配了本地队列，采用**工作窃取算法（Work-Stealing）**和 CPU 绑核来最大化缓存命中率。”

2. **第二层：范式跃迁（Data-Driven）**
   > “但解决调度只是第一步，传统协程只能阻塞在 Socket 等网络 IO 上。为了支撑高性能计算（如机器人/自动驾驶），我设计了 **DataDispatcher（数据分发中枢）**。协程不再绑定套接字，而是绑定业务 Channel。如果数据没准备好，协程主动挂起进入 `DATA_WAIT` 状态；当上游写入数据时，`DataNotifier` 会无锁地、瞬间唤醒对应的协程，实现了纯正的**数据流驱动计算**。”

3. **第三层：跨越边界（Transport / SHM）**
   > “为了解决多进程架构下的通信瓶颈，我实现了跨进程的零拷贝链路。底层用 `shm_open+mmap` 分配共享内存，但**没有使用传统的进程间锁**，而是用原子的 Indicator 维护读写位点。
   > **最巧妙的地方在于：** 我利用了 Linux 的 `eventfd` 来做跨进程提醒，**并把这个 `eventfd` 注册进了我底层协程框架的 `epoll` 树中**。这样，共享内存的数据到达，就像普通的网络包到达一样，直接在微秒级唤醒了挂起在 `epoll_wait` 上的协程。”

4. **第四层：架构抽象（Topology & API）**
   > “底层搞定后，为了好用，我在顶层抽象了有向图（Graph）作为计算图的拓扑管理。业务代码只需要写 `CreateWriter` 和 `CreateReader`，框架会自动根据 Topology 判断目标是在本进程还是其他进程，**如果是同进程就路由给 INTRA（指针直接投递），跨进程就路由给 SHM，对开发者完全透明**。”

5. **第五层：业务高光（DataFusion）**
   > “顺带提一句，针对自动驾驶中常见的传感器融合问题，我还在数据层开发了一个 `DataFusion` 组件。它支持多个通道的 Barrier 等待（比如同时等待 Camera 和 Lidar 的数据），只有所有数据对齐了，关联的融合协程才会被唤醒执行。
   > **最终测试下来，通过无锁化和 SHM，框架的跨进程往返延迟压到了 X 微秒级别。**”

按照这条路线，你展现出来的就不仅仅是“我会写代码”，而是**“我懂架构设计，我懂底层 OS 原理，我懂锁的代价并有能力消除它”**，这正是核心研发部门最看重的素质。


# MiniCyber / Cyber-Lite：精简版 CyberRT 实施路线图

## 面试口径（背下来）

> "这是一个参考百度 Apollo CyberRT 架构设计的自动驾驶/机器人高性能中间件。剥离了 FastRTPS 和 Protobuf 依赖，提取了最核心的**数据驱动协程调度器**与**零拷贝传输层**。包含无锁组件、基于 Classic 策略的工作窃取调度、数据分发中枢，以及跨进程共享内存通信底座。"

---

## Phase 1: 底层并发与无锁基础设施（约 600 行）

### Step 01: feat(base): 移植并发宏定义与内存工具

**目标**：建立底层宏体系，消除魔法数字，为后续无锁结构提供编译器优化提示。

**参考源码**：`cyber/base/macros.h`

**文件列表**：
- `include/minicyber/base/macros.h`（新增）
- `include/minicyber/common/types.h`（新增）
- `CMakeLists.txt`（修改，添加 include 目录）

**具体实现要点**：

1. **新增 `include/minicyber/base/macros.h`**：
   ```cpp
   #define cyber_likely(x)   __builtin_expect(!!(x), 1)
   #define cyber_unlikely(x) __builtin_expect(!!(x), 0)
   #define CACHELINE_SIZE 64
   
   inline void* CheckedMalloc(size_t size) {
     void* ptr = std::malloc(size);
     if (cyber_unlikely(!ptr)) throw std::bad_alloc();
     return ptr;
   }
   inline void* CheckedCalloc(size_t num, size_t size) {
     void* ptr = std::calloc(num, size);
     if (cyber_unlikely(!ptr)) throw std::bad_alloc();
     return ptr;
   }
   ```

2. **新增 `include/minicyber/common/types.h`**：
   ```cpp
   struct NullType {};
   enum class ReturnCode : int32_t { OK = 0, ERROR = -1, TIMEOUT = 1 };
   enum Relation { NO_RELATION, DIFF_PROC, DIFF_HOST, SAME_PROC };
   ```

3. **修改 `CMakeLists.txt`**：添加 `include_directories(${CMAKE_SOURCE_DIR}/include)`。

**提交信息**：
```
feat(base): 移植并发宏定义与内存工具

- 移植 cyber_likely/unlikely、CACHELINE_SIZE 缓存行对齐
- 添加 CheckedMalloc/CheckedCalloc 安全内存分配
- 定义 ReturnCode、Relation 等基础类型
- 零依赖，仅添加头文件基础设施
```

---

### Step 02: feat(base): 移植 WaitStrategy 等待策略

**目标**：为 BoundedQueue 提供可插拔的等待策略，支持阻塞、自旋、超时等多种场景。

**参考源码**：`cyber/base/wait_strategy.h`

**文件列表**：
- `include/minicyber/base/wait_strategy.h`（新增）
- `tests/test_wait_strategy.cpp`（新增）

**具体实现要点**：

1. **新增 `include/minicyber/base/wait_strategy.h`**：
   ```cpp
   class WaitStrategy {
   public:
     virtual void NotifyOne() {}
     virtual void BreakAllWait() {}
     virtual bool EmptyWait() { return true; }
     virtual ~WaitStrategy() = default;
   };
   class BlockWaitStrategy : public WaitStrategy {
     std::mutex mutex_; std::condition_variable cv_;
   public:
     void NotifyOne() override { cv_.notify_one(); }
     void BreakAllWait() override { cv_.notify_all(); }
     bool EmptyWait() override { std::unique_lock<std::mutex> lock(mutex_); cv_.wait(lock); return true; }
   };
   class SleepWaitStrategy : public WaitStrategy {
   public:
     bool EmptyWait() override { std::this_thread::sleep_for(std::chrono::milliseconds(1)); return true; }
   };
   class YieldWaitStrategy : public WaitStrategy {
   public:
     bool EmptyWait() override { std::this_thread::yield(); return true; }
   };
   ```

2. **新增测试**：验证各策略的 `EmptyWait` + `NotifyOne` 唤醒行为。

**提交信息**：
```
feat(base): 移植 WaitStrategy 等待策略

- 实现 BlockWaitStrategy（mutex + condition_variable）
- 实现 SleepWaitStrategy（1ms 睡眠）、YieldWaitStrategy（主动让出）
- 为后续 BoundedQueue 提供可插拔等待后端
- 添加单测验证阻塞与唤醒
```

---

### Step 03: feat(base): 移植无锁有界队列 BoundedQueue

**目标**：手写工业级无锁环形队列，消除全局锁竞争与伪共享。

**参考源码**：`cyber/base/bounded_queue.h`

**文件列表**：
- `include/minicyber/base/bounded_queue.h`（新增）
- `tests/test_bounded_queue.cpp`（新增）

**具体实现要点**：

1. **新增 `include/minicyber/base/bounded_queue.h`**：
   - 模板类 `BoundedQueue<T>`，内部 `T* pool_`（`CheckedMalloc` 分配）。
   - 核心原子变量按 `CACHELINE_SIZE` 对齐隔离：
     ```cpp
     alignas(CACHELINE_SIZE) std::atomic<uint64_t> head_{0};
     alignas(CACHELINE_SIZE) std::atomic<uint64_t> tail_{0};
     alignas(CACHELINE_SIZE) std::atomic<uint64_t> commit_{0};
     ```
   - 接口：`Init(size)`, `Init(size, WaitStrategy*)`, `Enqueue(const T&) -> bool`, `WaitEnqueue(const T&) -> bool`, `Dequeue(T*) -> bool`, `WaitDequeue(T*) -> bool`, `Size()`, `Empty()`, `BreakAllWait()`。
   - `Enqueue` 使用 CAS 更新 `tail_`，`commit_` 保证可见性。

2. **新增测试**：单线程顺序、多线程 1P1C 100w 次、容量边界、阻塞等待。

**提交信息**：
```
feat(base): 移植无锁有界队列 BoundedQueue

- 手写 head_/tail_/commit_ 三原子变量环形队列
- 成员变量按 CACHELINE_SIZE 对齐，消除伪共享
- 支持 Enqueue/Dequeue 的阻塞与非阻塞版本
- 添加并发测试：单线程、多线程、容量边界、阻塞唤醒
```

---

### Step 04: feat(base): 移植无锁读写锁 AtomicRWLock

**目标**：提供用户态读写锁，避免 `pthread_rwlock` 的系统调用开销。

**参考源码**：`cyber/base/atomic_rw_lock.h`

**文件列表**：
- `include/minicyber/base/atomic_rw_lock.h`（新增）
- `tests/test_atomic_rw_lock.cpp`（新增）

**具体实现要点**：

1. **新增 `include/minicyber/base/atomic_rw_lock.h`**：
   ```cpp
   class AtomicRWLock {
   public:
     void ReadLock() {
       uint32_t retry = 0;
       while (cyber_unlikely(!TryReadLock())) {
         if (++retry == 5) { retry = 0; std::this_thread::yield(); }
       }
     }
     void WriteLock() {
       uint32_t retry = 0;
       while (cyber_unlikely(!TryWriteLock())) {
         if (++retry == 5) { retry = 0; std::this_thread::yield(); }
       }
     }
     void ReadUnlock() { lock_num_.fetch_sub(2, std::memory_order_release); }
     void WriteUnlock() { lock_num_.fetch_and(0, std::memory_order_release); }
   private:
     bool TryReadLock() {
       int32_t expect = 0;
       return lock_num_.compare_exchange_weak(expect, expect + 2,
         std::memory_order_acq_rel, std::memory_order_relaxed);
     }
     bool TryWriteLock() {
       int32_t expect = 0;
       return lock_num_.compare_exchange_weak(expect, 1,
         std::memory_order_acq_rel, std::memory_order_relaxed);
     }
     alignas(CACHELINE_SIZE) std::atomic<int32_t> lock_num_{0};
   };
   ```

2. **新增测试**：多读者并发、写者独占、读写交替。

**提交信息**：
```
feat(base): 移植无锁读写锁 AtomicRWLock

- 基于 atomic<int32_t> 的用户态自旋读写锁
- 读锁 +2，写锁设为 1，使用 memory_order_acq_rel 保证序
- 自旋 5 次后 yield，避免 CPU 空转
- 添加并发读写测试
```

---

### Step 05: feat(base): 移植固定大小无锁哈希 AtomicHashMap

**目标**：引入无锁哈希表，为后续 DataDispatcher 优化做准备。先引入不替换，保证可编译。

**参考源码**：`cyber/base/atomic_hash_map.h`

**文件列表**：
- `include/minicyber/base/atomic_hash_map.h`（新增）
- `tests/test_atomic_hash_map.cpp`（新增）

**具体实现要点**：

1. **新增 `include/minicyber/base/atomic_hash_map.h`**：
   - 模板参数 `K, V, TableSize, Hash = std::hash<K>>`。
   - 使用 `std::array<std::atomic<Node*>, TableSize>`，`Node` 为链表节点。
   - 接口：`Get(key, *value) -> bool`, `Set(key, value)`, `Has(key) -> bool`, `Remove(key) -> bool`。
   - 冲突处理：CAS 链表头插入。

2. **新增测试**：单线程 CRUD、多线程并发 Set + Get。

**提交信息**：
```
feat(base): 移植固定大小无锁哈希 AtomicHashMap

- 基于 CAS 链表处理哈希冲突
- 模板参数 TableSize 固定桶数量，适用于 channel 数量有限场景
- 先引入并验证正确性，暂不替换现有 map
- 添加并发 CRUD 测试
```

---

## Phase 2: CyberRT 协程与调度引擎（约 1000 行）

### Step 06: refactor(croutine): 对齐 CyberRT 协程状态机

**目标**：将协程状态扩展为 CyberRT 风格，支持 `DATA_WAIT` 等数据驱动状态。

**参考源码**：`cyber/croutine/croutine.h`

**文件列表**：
- `include/minicyber/croutine/croutine.h`（新增/修改，从现有 fiber.h 演进）
- `src/croutine/croutine.cpp`（新增/修改）
- `tests/test_croutine.cpp`（新增）

**具体实现要点**：

1. **状态枚举扩展**（保持旧值兼容）：
   ```cpp
   enum class RoutineState {
     READY = 0, FINISHED, SLEEP, IO_WAIT, DATA_WAIT
   };
   ```

2. **新增接口**：
   ```cpp
   class CRoutine {
   public:
     RoutineState State() const;
     void SetState(RoutineState state);
     static void Yield();
     static void Yield(const RoutineState& state); // 让出并设置目标状态
   };
   ```

3. **`Yield` 实现**：根据目标状态设置状态并 `swapOut()`。

4. **测试**：验证状态流转 `READY -> EXEC -> DATA_WAIT -> READY -> FINISHED`。

**提交信息**：
```
refactor(croutine): 对齐 CyberRT 协程状态机

- 引入 RoutineState：READY, FINISHED, SLEEP, IO_WAIT, DATA_WAIT
- 保留旧状态值兼容，新增 Yield(state) 重载
- 支持协程因数据未就绪而主动挂起（DATA_WAIT）
- 添加状态流转测试
```

---

### Step 07: feat(scheduler): 移植 CPU 亲和性与调度策略封装

**目标**：支持线程绑核与调度策略设置，减少 CPU 迁移开销。

**参考源码**：`cyber/scheduler/common/pin_thread.h`

**文件列表**：
- `include/minicyber/scheduler/common/pin_thread.h`（新增）
- `src/scheduler/common/pin_thread.cpp`（新增）
- `tests/test_pin_thread.cpp`（新增）

**具体实现要点**：

1. **新增 `include/minicyber/scheduler/common/pin_thread.h`**：
   ```cpp
   bool PinThread(pthread_t tid, const std::vector<int>& cpuset);
   bool SetThreadName(pthread_t tid, const std::string& name);
   bool SetSchedPolicy(pthread_t tid, int policy, int prio);
   ```

2. **实现 `PinThread`**：使用 `pthread_setaffinity_np` 或 `sched_setaffinity`。

3. **测试**：验证绑核后线程运行在指定 CPU。

**提交信息**：
```
feat(scheduler): 移植 CPU 亲和性与调度策略封装

- 实现 PinThread：pthread_setaffinity_np 绑核
- 实现 SetThreadName / SetSchedPolicy
- 为后续 Processor 线程绑定提供工具
- 添加绑核功能测试
```

---

### Step 08: feat(scheduler): 移植 Processor 与 ProcessorContext 接口

**目标**：抽象处理器线程和上下文切换接口，为 Classic 调度做准备。

**参考源码**：`cyber/scheduler/processor.h`, `processor_context.h`

**文件列表**：
- `include/minicyber/scheduler/processor.h`（新增）
- `include/minicyber/scheduler/processor_context.h`（新增）
- `src/scheduler/processor.cpp`（新增）
- `tests/test_processor.cpp`（新增）

**具体实现要点**：

1. **新增 `ProcessorContext`**：
   ```cpp
   class ProcessorContext {
   public:
     virtual std::shared_ptr<CRoutine> NextRoutine() = 0;
     virtual void Wait() = 0;
     virtual void Shutdown() = 0;
   };
   ```

2. **新增 `Processor`**：
   ```cpp
   class Processor {
   public:
     void Start();
     void Stop();
     void BindContext(const std::shared_ptr<ProcessorContext>& ctx);
   private:
     std::shared_ptr<ProcessorContext> context_;
     std::thread thread_;
     std::atomic<bool> running_{false};
   };
   ```
   - `Start()` 创建线程，循环执行 `context_->NextRoutine()`，无任务时 `context_->Wait()`。

3. **测试**：验证 Processor 绑定 Mock Context 后正确调度。

**提交信息**：
```
feat(scheduler): 移植 Processor 与 ProcessorContext 接口

- ProcessorContext 抽象：NextRoutine()、Wait()、Shutdown()
- Processor 封装 std::thread，绑定 Context 后循环调度
- 无任务时调用 Wait() 避免空转
- 添加 Mock Context 测试验证调度循环
```

---

### Step 09: feat(scheduler): 移植 Classic 调度上下文与本地队列

**目标**：实现按优先级划分的多级队列，并为每个 Processor 引入本地任务队列。

**参考源码**：`cyber/scheduler/policy/classic_context.h`

**文件列表**：
- `include/minicyber/scheduler/policy/classic_context.h`（新增）
- `src/scheduler/policy/classic_context.cpp`（新增）
- `include/minicyber/scheduler/scheduler_conf.h`（新增）
- `tests/test_classic_context.cpp`（新增）

**具体实现要点**：

1. **新增 `SchedulerConf`**：
   ```cpp
   struct SchedulerConf {
     uint32_t thread_num = 0;
     std::string policy = "classic";
     bool affinity = false;
     std::vector<int> cpuset;
     std::vector<uint32_t> prio_threshold; // 优先级阈值
   };
   ```

2. **新增 `ClassicContext`**：
   - 多级优先级队列（0-19），每级一个 `std::deque<std::shared_ptr<CRoutine>>`。
   - 每个 Processor 拥有独立的 `ClassicContext` 实例（含本地队列）。
   - `NextRoutine()`：从高优先级到低优先级扫描本地队列。
   - `Wait()`：使用 `BlockWaitStrategy` 阻塞等待。

3. **本地队列**：每个 Context 持有 `std::deque`，配 `std::mutex`（简化版，面试可解释）。

**提交信息**：
```
feat(scheduler): 移植 Classic 调度上下文与本地队列

- 实现 0-19 多级优先级队列（MULTI_PRIO_QUEUE）
- 每个 Processor 绑定独立 ClassicContext，拥有本地任务队列
- NextRoutine() 按优先级从高到低扫描
- Wait() 使用 BlockWaitStrategy 阻塞
- 添加多级优先级调度测试
```

---

### Step 10: feat(scheduler): 实现 Scheduler 顶层封装与工作窃取

**目标**：整合 Processor、ClassicContext，实现 Work-Stealing 负载均衡。

**参考源码**：`cyber/scheduler/scheduler.h`

**文件列表**：
- `include/minicyber/scheduler/scheduler.h`（新增）
- `src/scheduler/scheduler.cpp`（新增）
- `tests/test_scheduler.cpp`（新增）

**具体实现要点**：

1. **新增 `Scheduler`**：
   ```cpp
   class Scheduler {
   public:
     explicit Scheduler(const SchedulerConf& conf);
     void CreateTask(const std::function<void()>& func, const std::string& name, uint32_t prio = 0);
     void NotifyTask(const std::shared_ptr<CRoutine>& rt);
     void Shutdown();
   private:
     std::vector<std::shared_ptr<Processor>> processors_;
     std::vector<std::shared_ptr<ClassicContext>> contexts_;
     std::shared_ptr<CRoutine> Steal(uint32_t processor_id);
   };
   ```

2. **Work-Stealing 实现**：
   - `Steal()`：随机选择其他 Processor 的 Context，从其本地队列尾部偷取。
   - **使用自旋锁**：`std::mutex` 保护本地队列，面试口径："Steal 发生频率极低，自旋锁足够且实现简单"。
   - `Processor` 的 `NextRoutine()` 扩展：本地空 → 全局空 → Steal → Wait。

3. **CPU 亲和性**：构造时根据 `conf.affinity` 和 `conf.cpuset` 为每个 Processor 线程调用 `PinThread`。

**提交信息**：
```
feat(scheduler): 实现 Scheduler 顶层封装与工作窃取

- 整合 Processor + ClassicContext，提供 CreateTask/NotifyTask 接口
- Work-Stealing：随机选择其他 Processor，从尾部偷取任务
- 本地队列使用 mutex 保护（Steal 频率低，自旋锁足够）
- 支持根据配置自动绑核
- 添加多任务负载均衡测试
```

---

## Phase 3: 数据驱动中枢（Data 层）（约 1000 行）

### Step 11: feat(data): 移植 CacheBuffer 环形缓存

**目标**：实现覆盖写环形缓冲，支持历史数据读取与最新值获取。

**参考源码**：`cyber/data/cache_buffer.h`

**文件列表**：
- `include/minicyber/data/cache_buffer.h`（新增）
- `tests/test_cache_buffer.cpp`（新增）

**具体实现要点**：

1. **新增 `CacheBuffer<T>`**：
   ```cpp
   template <typename T>
   class CacheBuffer {
   public:
     explicit CacheBuffer(uint64_t capacity = 1);
     void Fill(const T& value);
     const T& Fetch(uint64_t index) const;
     const T& Latest() const;
     uint64_t Head() const;
     uint64_t Tail() const;
     uint64_t Size() const;
     bool Empty() const;
   private:
     std::vector<T> buffer_;
     uint64_t capacity_;
     std::atomic<uint64_t> head_{0};
     std::atomic<uint64_t> tail_{0};
   };
   ```

2. **测试**：Fill 覆盖写、Latest 正确性、并发 Fill 安全。

**提交信息**：
```
feat(data): 移植 CacheBuffer 环形缓存

- 覆盖写环形缓冲，head_/tail_ 使用 atomic 保证多线程安全
- 支持 Fetch(index)、Latest()、Head()、Tail()、Size()
- 添加单线程与并发 Fill 测试
```

---

### Step 12: feat(data): 移植 ChannelBuffer 通道缓冲

**目标**：将 CacheBuffer 与 channel_id 绑定，实现按通道隔离。

**参考源码**：`cyber/data/channel_buffer.h`

**文件列表**：
- `include/minicyber/data/channel_buffer.h`（新增）
- `tests/test_channel_buffer.cpp`（新增）

**具体实现要点**：

1. **新增 `ChannelBuffer<T>`**：
   ```cpp
   template <typename T>
   class ChannelBuffer {
   public:
     ChannelBuffer(uint64_t channel_id, uint64_t capacity);
     bool Fetch(uint64_t index, T* m);
     bool Latest(T* m);
     bool FetchMulti(uint64_t index, std::vector<T>* vec);
     void Fill(const T& msg);
     uint64_t ChannelId() const;
   private:
     uint64_t channel_id_;
     CacheBuffer<T> buffer_;
   };
   ```

**提交信息**：
```
feat(data): 移植 ChannelBuffer 通道缓冲

- 包装 CacheBuffer，增加 channel_id_ 标识
- 提供 Fetch(index)、Latest()、FetchMulti() 接口
- 返回 bool 表示数据有效性
- 添加通道隔离与数据读写测试
```

---

### Step 13: feat(data): 移植 DataNotifier 唤醒通知器

**目标**：实现"数据到达时唤醒等待协程"的核心机制。

**参考源码**：`cyber/data/data_notifier.h`

**文件列表**：
- `include/minicyber/data/data_notifier.h`（新增）
- `tests/test_data_notifier.cpp`（新增）

**具体实现要点**：

1. **新增 `DataNotifier`**：
   ```cpp
   struct Notifier {
     std::function<void()> callback;
     uint64_t next_index = 0;
   };
   
   class DataNotifier {
   public:
     static DataNotifier* Instance();
     bool AddNotifier(uint64_t channel_id, const Notifier& notifier);
     bool Notify(uint64_t channel_id);
     void Shutdown();
   private:
     std::unordered_map<uint64_t, std::vector<Notifier>> notifiers_map_;
     std::mutex notifiers_map_mutex_;
   };
   ```

2. **关键**：Notifier 的 callback 由 Reader 在创建时绑定为 `scheduler->schedule(fiber)`，实现 DATA_WAIT → READY 的唤醒。

**提交信息**：
```
feat(data): 移植 DataNotifier 唤醒通知器

- channel_id -> Notifier 列表映射
- Notifier 包含 callback 和 next_index
- Notify() 遍历并触发该 channel 所有回调
- 回调由上层 Reader 绑定 scheduler->schedule，实现协程唤醒
- 添加回调触发与多订阅者测试
```

---

### Step 14: feat(data): 移植 DataDispatcher 分发器（shared_mutex 版）

**目标**：实现数据写入 ChannelBuffer 并触发通知的统一入口。先用 `std::shared_mutex` 保证正确性，后续再优化为无锁。

**参考源码**：`cyber/data/data_dispatcher.h`

**文件列表**：
- `include/minicyber/data/data_dispatcher.h`（新增）
- `tests/test_data_dispatcher.cpp`（新增）

**具体实现要点**：

1. **新增 `DataDispatcher<T>`**：
   ```cpp
   template <typename T>
   class DataDispatcher {
   public:
     using BufferVector = std::vector<std::shared_ptr<ChannelBuffer<T>>>;
     static DataDispatcher<T>* Instance();
     void AddBuffer(uint64_t channel_id, const std::shared_ptr<ChannelBuffer<T>>& buffer);
     bool Dispatch(uint64_t channel_id, const T& msg);
   private:
     std::unordered_map<uint64_t, BufferVector> buffers_map_;
     std::shared_mutex buffers_map_mutex_;  // C++14 读写锁
     DataNotifier* notifier_ = DataNotifier::Instance();
   };
   ```

2. **`Dispatch` 逻辑**：
   - `shared_lock` 读 `buffers_map_`。
   - 遍历所有 buffer 执行 `Fill(msg)`。
   - 调用 `notifier_->Notify(channel_id)`。

3. **设计决策**：先用 `shared_mutex`，调试压力小；后续 Step 26 替换为 `AtomicHashMap`。

**提交信息**：
```
feat(data): 移植 DataDispatcher 分发器（shared_mutex 版）

- 单例模板类，按 channel_id 管理 ChannelBuffer 列表
- Dispatch()：shared_lock 读 buffers_map_，Fill 所有 buffer，然后 Notify
- 先使用 std::shared_mutex 保证正确性，降低调试难度
- 添加数据分发与通知联动测试
```

---

### Step 15: feat(data): 移植 DataVisitor 数据访问器

**目标**：封装协程对数据的访问，无数据时让协程进入 `DATA_WAIT`。

**参考源码**：`cyber/data/data_visitor.h`

**文件列表**：
- `include/minicyber/data/data_visitor.h`（新增）
- `tests/test_data_visitor.cpp`（新增）

**具体实现要点**：

1. **新增 `DataVisitor<T>`**：
   ```cpp
   template <typename T>
   class DataVisitor {
   public:
     DataVisitor(uint64_t channel_id, uint64_t capacity = 10);
     bool TryFetch(T* msg);      // 非阻塞，有数据返回 true
     bool Fetch(T* msg);         // 阻塞，无数据时让出协程进入 DATA_WAIT
   private:
     uint64_t channel_id_;
     std::shared_ptr<ChannelBuffer<T>> buffer_;
   };
   ```

2. **`Fetch` 实现**：
   ```cpp
   bool Fetch(T* msg) {
     while (!buffer_->Latest(msg)) {
       auto rt = CRoutine::GetThis();
       rt->SetState(RoutineState::DATA_WAIT);
       Scheduler::GetThis()->WaitForData(rt, channel_id_);
     }
     return true;
   }
   ```

**提交信息**：
```
feat(data): 移植 DataVisitor 数据访问器

- TryFetch() 非阻塞取数据
- Fetch() 无数据时设置 DATA_WAIT 并 Yield，等待 DataNotifier 唤醒
- 实现协程因业务数据未就绪而主动挂起的核心机制
- 添加阻塞与非阻塞数据访问测试
```

---

### Step 16: feat(data): 新增 DataFusion 多通道屏障等待

**目标**：支持多传感器数据时间戳对齐，两个通道数据都 Ready 才唤醒协程。简历亮点。

**参考源码**：`cyber/data/fusion/all_latest.h`（简化版）

**文件列表**：
- `include/minicyber/data/data_fusion.h`（新增）
- `tests/test_data_fusion.cpp`（新增）

**具体实现要点**：

1. **新增 `DataFusion`**：
   ```cpp
   template <typename T1, typename T2>
   class DataFusion {
   public:
     DataFusion(uint64_t ch1, uint64_t ch2);
     bool WaitForAllLatest(T1* msg1, T2* msg2); // 屏障等待
   private:
     uint64_t ch1_, ch2_;
     std::atomic<bool> ready1_{false}, ready2_{false};
     T1 cache1_; T2 cache2_;
   };
   ```

2. **实现**：内部注册两个 DataVisitor，使用条件变量或协程状态机实现"两个都到才唤醒"。

**提交信息**：
```
feat(data): 新增 DataFusion 多通道屏障等待

- 支持 WaitForAllLatest：两个通道数据都 Ready 才唤醒协程
- 参考 CyberRT AllLatest 策略，简化实现
- 满足自动驾驶多传感器时间戳对齐需求
- 添加双通道屏障等待测试
```

---

## Phase 4: 传输层 Transport（剥离 FastRTPS，实现 SHM）（约 1000 行）

### Step 17: feat(transport): 移植 SHM 基础内存结构

**目标**：定义共享内存的控制区、消息块和段接口。

**参考源码**：`cyber/transport/shm/state.h`, `block.h`, `segment.h`

**文件列表**：
- `include/minicyber/transport/shm/state.h`（新增）
- `include/minicyber/transport/shm/block.h`（新增）
- `include/minicyber/transport/shm/segment.h`（新增）

**具体实现要点**：

1. **新增 `State`**：全局控制区，管理 Block 分配状态。
2. **新增 `Block`**：带 `AtomicRWLock` 的消息块，存储实际 payload。
3. **新增 `Segment`** 接口：
   ```cpp
   class Segment {
   public:
     virtual bool Open() = 0;
     virtual void Close() = 0;
     virtual void* GetMemPtr() = 0;
     virtual size_t GetSize() = 0;
   };
   ```

**提交信息**：
```
feat(transport): 移植 SHM 基础内存结构

- State：全局控制区，管理 Block 分配
- Block：带 AtomicRWLock 的消息块
- Segment 抽象接口：Open/Close/GetMemPtr
- 为 PosixSegment 实现提供契约
```

---

### Step 18: feat(transport): 实现 PosixSegment 与生命周期管理

**目标**：基于 `shm_open + mmap` 实现物理共享内存，并解决生命周期泄漏问题。

**参考源码**：`cyber/transport/shm/posix_segment.cc`

**文件列表**：
- `include/minicyber/transport/shm/posix_segment.h`（新增）
- `src/transport/shm/posix_segment.cpp`（新增）
- `tests/test_posix_segment.cpp`（新增）

**具体实现要点**：

1. **新增 `PosixSegment`**：
   ```cpp
   class PosixSegment : public Segment {
   public:
     PosixSegment(const std::string& name, size_t size);
     bool Open() override;
     void Close() override;
   };
   ```

2. **生命周期管理**（坑点二）：
   - 在 `Open()` 中使用 `shm_open(name, O_CREAT | O_RDWR, 0666)` + `ftruncate` + `mmap`。
   - 注册 Signal Handler 捕获 `SIGINT/SIGTERM/SIGSEGV`，在进程退出时调用 `shm_unlink`。
   - 或使用文件锁检测残留 SHM 文件并清理。

3. **测试**：fork 父子进程通过 Segment 读写。

**提交信息**：
```
feat(transport): 实现 PosixSegment 与生命周期管理

- 基于 shm_open + mmap 实现物理共享内存
- 注册 Signal Handler 捕获 SIGINT/SIGTERM/SIGSEGV
- 进程异常退出时自动 shm_unlink，防止 /dev/shm 泄漏
- 添加 fork 跨进程读写测试
```

---

### Step 19: feat(transport): 移植 ConditionNotifier（eventfd + epoll 简化版）

**目标**：实现跨进程事件通知，将 eventfd 注册进 IOManager 的 epoll。

**参考源码**：`cyber/transport/shm/condition_notifier.cc`（简化）

**文件列表**：
- `include/minicyber/transport/shm/condition_notifier.h`（新增）
- `src/transport/shm/condition_notifier.cpp`（新增）
- `tests/test_condition_notifier.cpp`（新增）

**具体实现要点**：

1. **简化设计**：用 `eventfd` 替代 CyberRT 复杂的 UDP 组播。
   ```cpp
   class ConditionNotifier {
   public:
     bool Init();
     void Notify();        // write eventfd
     bool Listen(int timeout_ms = -1); // epoll_wait
     int Fd() const;
   private:
     int event_fd_ = -1;
     int epoll_fd_ = -1;
   };
   ```

2. **关键融合点（坑点三）**：`ShmReceiver` 中将 `ConditionNotifier::Fd()` 注册进 Sylar 的 `IOManager` epoll。当 eventfd 可读时，IOManager 回调读取 SHM 数据，再注入 `DataDispatcher`。

**提交信息**：
```
feat(transport): 移植 ConditionNotifier（eventfd + epoll 简化版）

- 用 eventfd 替代复杂组播，Notify 写 8 字节，Listen 通过 epoll 阻塞
- 关键设计：eventfd 注册进底层 IOManager 的 epoll
- 数据到达时 epoll_wait 唤醒，将 SHM 数据注入 DataDispatcher
- 添加跨进程事件通知测试
```

---

### Step 20: feat(transport): 移植 IntraDispatcher 与 ShmDispatcher

**目标**：实现同进程和跨进程两种分发后端。

**参考源码**：`cyber/transport/dispatcher/intra_dispatcher.h`, `shm_dispatcher.h`

**文件列表**：
- `include/minicyber/transport/dispatcher/intra_dispatcher.h`（新增）
- `include/minicyber/transport/dispatcher/shm_dispatcher.h`（新增）
- `src/transport/dispatcher/shm_dispatcher.cpp`（新增）
- `tests/test_dispatcher.cpp`（新增）

**具体实现要点**：

1. **新增 `IntraDispatcher`**：直接转发给 `DataDispatcher<T>::Instance()->Dispatch()`。

2. **新增 `ShmDispatcher`**：
   - 持有 `PosixSegment` + `ConditionNotifier`。
   - 后台协程（或线程）`epoll_wait` eventfd。
   - 收到通知后读取 Segment，反序列化（先支持 `std::string`），调用 `DataDispatcher::Dispatch` 注入进程内。

**提交信息**：
```
feat(transport): 移植 IntraDispatcher 与 ShmDispatcher

- IntraDispatcher：直接调用 DataDispatcher，指针级零拷贝
- ShmDispatcher：组合 PosixSegment + ConditionNotifier
- 后台协程 epoll_wait eventfd，收到信号后读 SHM 并注入 DataDispatcher
- 添加同进程与跨进程分发测试
```

---

### Step 21: feat(transport): 移植 Transmitter 与 Receiver 接口

**目标**：封装底层 Dispatcher，提供统一的发布/订阅接口。

**参考源码**：`cyber/transport/transmitter/*`, `cyber/transport/receiver/*`

**文件列表**：
- `include/minicyber/transport/transmitter/transmitter.h`（新增）
- `include/minicyber/transport/transmitter/intra_transmitter.h`（新增）
- `include/minicyber/transport/transmitter/shm_transmitter.h`（新增）
- `include/minicyber/transport/receiver/receiver.h`（新增）
- `include/minicyber/transport/receiver/shm_receiver.h`（新增）
- `tests/test_transceiver.cpp`（新增）

**具体实现要点**：

1. **Transmitter 体系**：
   - `IntraTransmitter`：直接调用 `IntraDispatcher::Dispatch`。
   - `ShmTransmitter`：序列化到 Segment，更新 Indicator，Notify eventfd。

2. **Receiver 体系**：
   - `ShmReceiver`：监听 eventfd（通过 IOManager），读取 Segment，回调给用户。

3. **序列化简化**：先支持 `std::string`，通过 `memcpy` 拷贝到 Segment。面试口径："预留了二进制序列化接口，当前用 string 做演示"。

**提交信息**：
```
feat(transport): 移植 Transmitter 与 Receiver 接口

- IntraTransmitter：直接调 IntraDispatcher
- ShmTransmitter：拷贝到 Segment + 唤醒 eventfd
- ShmReceiver：通过 IOManager 监听 eventfd，读取后回调
- 先支持 std::string 传输，预留二进制序列化接口
- 添加跨进程收发测试
```

---

## Phase 5: 高层 API、拓扑与收尾（约 600 行）

### Step 22: feat(topology): 移植轻量级 TopologyManager

**目标**：管理节点-通道订阅关系，支持同进程/跨进程判断。

**参考源码**：`cyber/service_discovery/topology_manager.h`, `container/graph.h`

**文件列表**：
- `include/minicyber/topology/topology_manager.h`（新增）
- `src/topology/topology_manager.cpp`（新增）
- `tests/test_topology.cpp`（新增）

**具体实现要点**：

1. **新增 `TopologyManager`**：
   ```cpp
   class TopologyManager {
   public:
     static TopologyManager* Instance();
     void AddNode(const std::string& name, pid_t pid = getpid());
     void AddChannelReader(const std::string& channel, const std::string& node, pid_t pid);
     void AddChannelWriter(const std::string& channel, const std::string& node, pid_t pid);
     bool IsSameProc(const std::string& channel) const;
     std::string DumpGraph() const; // DOT 格式
   private:
     std::unordered_map<std::string, ChannelInfo> channels_;
     mutable std::mutex mutex_;
   };
   ```

**提交信息**：
```
feat(topology): 移植轻量级 TopologyManager

- 管理 channel -> {publishers, subscribers, pids} 关系
- IsSameProc() 判断通道是否仅在本进程内通信
- DumpGraph() 输出 DOT 格式有向图
- 添加拓扑注册与同进程判断测试
```

---

### Step 23: feat(transport): 移植 Transport 顶层路由

**目标**：根据 TopologyManager 自动选择 INTRA 或 SHM 后端。

**参考源码**：`cyber/transport/transport.h`

**文件列表**：
- `include/minicyber/transport/transport.h`（新增）
- `tests/test_transport_routing.cpp`（新增）

**具体实现要点**：

1. **新增 `Transport`**：
   ```cpp
   class Transport {
   public:
     template <typename T>
     static std::shared_ptr<Transmitter<T>> CreateTransmitter(const std::string& channel);
     template <typename T>
     static std::shared_ptr<Receiver<T>> CreateReceiver(const std::string& channel);
   private:
     static bool UseShm(const std::string& channel);
   };
   ```
   - `UseShm`：查询 `TopologyManager::IsSameProc(channel)`。

**提交信息**：
```
feat(transport): 移植 Transport 顶层路由

- 工厂模式自动创建 Intra 或 Shm 的 Transmitter/Receiver
- 同进程走 DataDispatcher（零拷贝）
- 跨进程走 ShmTransmitter/ShmReceiver（共享内存）
- 对上层完全透明，用户无感知
- 添加自动路由选择测试
```

---

### Step 24: feat(node): 移植 Node、Reader、Writer 封装

**目标**：提供对标 CyberRT 的优雅用户接口。

**参考源码**：`cyber/node/node.h`, `reader.h`, `writer.h`

**文件列表**：
- `include/minicyber/node/node.h`（新增）
- `include/minicyber/node/reader.h`（新增）
- `include/minicyber/node/writer.h`（新增）
- `tests/test_node.cpp`（新增）

**具体实现要点**：

1. **新增 `Node`**：
   ```cpp
   class Node {
   public:
     explicit Node(const std::string& name);
     template <typename T>
     std::shared_ptr<Reader<T>> CreateReader(const std::string& channel);
     template <typename T>
     std::shared_ptr<Writer<T>> CreateWriter(const std::string& channel);
   };
   ```

2. **Reader/Writer**：内部组合 `DataVisitor` / `Transport`，创建时自动注册拓扑。

**提交信息**：
```
feat(node): 移植 Node、Reader、Writer 封装

- Node 作为 Reader/Writer 工厂和生命周期管理者
- CreateReader 内部创建 DataVisitor 并关联协程
- 创建时自动向 TopologyManager 注册拓扑关系
- 添加 Node 创建与销毁测试
```

---

### Step 25: feat(examples): 移植 Talker/Listener 官方 Demo

**目标**：展示最终 API 的工业美感。

**参考源码**：`examples/cyber/talker.cc`, `listener.cc`

**文件列表**：
- `examples/talker.cpp`（新增）
- `examples/listener.cpp`（新增）
- `CMakeLists.txt`（修改，添加 examples）

**具体实现要点**：

1. **talker.cpp**：Node + Writer，每秒发布消息。
2. **listener.cpp**：Node + Reader，回调打印。

**提交信息**：
```
feat(examples): 移植 Talker/Listener 官方 Demo

- talker：Node + Writer，定时发布字符串消息
- listener：Node + Reader，收到后打印
- 展示 CyberRT 风格的 Node/Reader/Writer 编程模型
- 可直接运行验证同进程通信
```

---

### Step 26: perf(data): 用 AtomicHashMap 替换 DataDispatcher 中的 shared_mutex

**目标**：消除 DataDispatcher 的最后一处锁竞争，完成无锁化。

**文件列表**：
- `include/minicyber/data/data_dispatcher.h`（修改）
- `tests/test_data_dispatcher.cpp`（保持，验证替换后行为一致）

**具体实现要点**：

1. **替换**：
   ```cpp
   // 删除
   // std::unordered_map<uint64_t, BufferVector> buffers_map_;
   // std::shared_mutex buffers_map_mutex_;
   
   // 新增
   AtomicHashMap<uint64_t, BufferVector, 256> buffers_map_;
   ```

2. **`Dispatch` 改为无锁读取**：
   ```cpp
   BufferVector* buffers = nullptr;
   if (!buffers_map_.Get(channel_id, &buffers)) return false;
   for (auto& buf : *buffers) buf->Fill(msg);
   notifier_->Notify(channel_id);
   ```

**提交信息**：
```
perf(data): 用 AtomicHashMap 替换 DataDispatcher 中的 shared_mutex

- 移除 std::shared_mutex，改用 Phase 1 引入的 AtomicHashMap
- Dispatch() 零锁竞争，仅依赖 ChannelBuffer 内部原子操作
- 所有现有测试保持通过
- 完成数据分发层的无锁化
```

---

### Step 27: docs: 补充 README 与压测数据

**目标**：文档化项目，量化性能指标。

**文件列表**：
- `README.md`（新增）
- `examples/benchmark_pingpong.cpp`（新增）

**具体实现要点**：

1. **README.md**：
   - 项目简介、架构图（ASCII）、编译方式。
   - CyberRT 源码对应索引表。
   - 性能数据：进程内 100w 消息 QPS、跨进程 1MB SHM 吞吐量。

2. **benchmark_pingpong.cpp**：
   - 协程 INTRA vs 线程 PIPE 对比。
   - 使用 `Time::MonoTime()` 纳秒级计时。

**提交信息**：
```
docs: 补充 README 与压测数据

- 编写项目简介、ASCII 架构图、编译指南
- 添加 CyberRT 源码对应索引表
- 实现 benchmark_pingpong：协程 INTRA vs 线程 PIPE
- 量化性能指标：平均延迟、QPS、吞吐量
```

---

## 最终 Step 清单（27 个）

| Phase | Step | 核心内容 |
|-------|--------|----------|
| **P1 基础设施** | 01 | 基础宏与类型 |
| | 02 | WaitStrategy |
| | 03 | BoundedQueue |
| | 04 | AtomicRWLock |
| | 05 | AtomicHashMap |
| **P2 调度引擎** | 06 | 协程状态机（DATA_WAIT） |
| | 07 | CPU 亲和性 |
| | 08 | Processor/Context |
| | 09 | Classic 调度 + 本地队列 |
| | 10 | Scheduler + Work-Stealing |
| **P3 数据中枢** | 11 | CacheBuffer |
| | 12 | ChannelBuffer |
| | 13 | DataNotifier |
| | 14 | DataDispatcher（shared_mutex） |
| | 15 | DataVisitor |
| | 16 | DataFusion（新增亮点） |
| **P4 传输层** | 17 | SHM State/Block/Segment |
| | 18 | PosixSegment + 生命周期 |
| | 19 | ConditionNotifier（eventfd） |
| | 20 | Intra/Shm Dispatcher |
| | 21 | Transmitter/Receiver |
| **P5 高层与收尾** | 22 | TopologyManager |
| | 23 | Transport 自动路由 |
| | 24 | Node/Reader/Writer |
| | 25 | Talker/Listener Demo |
| | 26 | AtomicHashMap 替换优化 |
| | 27 | README + Benchmark |

**代码总量预估**：4000-5000 行（不含测试），完美命中甜点区。
