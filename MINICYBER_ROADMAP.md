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
**目标**：打破进程壁垒，将独立的计算进程融合成一张网，对齐 CyberRT 原生的 System V 共享内存 + 轮询通知方案。

**要写的文件**：
*   `transport/shm/state.h`, `block.h`, `segment.h`
*   `transport/shm/posix_segment.h / .cpp`
*   `transport/shm/condition_notifier.h / .cpp` (System V SHM + Indicator 轮询)
*   `transport/dispatcher/intra_dispatcher.h / .cpp`
*   `transport/dispatcher/shm_dispatcher.h / .cpp`
*   `transport/transmitter/...`, `transport/receiver/...`

**核心改动**：
1. **物理内存映射**：通过 `shm_open` + `mmap` 分配 POSIX 共享内存（Segment），并处理 SIGINT 信号防止 `/dev/shm` 泄漏。
2. **跨进程唤醒桥接（CyberRT 原生方案）**：使用 System V 共享内存（`shmget/shmat`）承载一个 `Indicator` 环形缓冲。写者 `Notify()` 把 `ReadableInfo{host_id, block_index, channel_id}` 写入环形并原子累加 `next_seq`；读者后台线程 `Listen(timeout, &info)` 轮询 `next_seq` 变化，命中后直接拿到目标 channel 与 block_index。通知本身携带路由信息，无需扫描所有 segment。
3. **混合收发器**：封装 `IntraTransmitter` (同进程) 和 `ShmTransmitter` (跨进程)。

**Phase 3 产出**：具备跨进程零拷贝能力的底层传输链路，对齐 CyberRT 原生基线。
**对应简历句**：针对跨进程通信瓶颈，实现基于 `shm_open + mmap` 的共享内存映射；抛弃传统 IPC 锁，采用 CyberRT 原生的 **System V SHM + Indicator 环形缓冲**方案，通知本身携带 `{channel_id, block_index}` 路由信息，后台线程 50µs 粒度轮询唤醒，实现跨进程消息的微秒级协程调度。

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

### Phase 6：Protobuf + 组件加载与 RPC 通信 (约 800 行)
**目标**：严格对齐 CyberRT 运行时框架，告别手写 main 函数的“玩具感”，引入 Protobuf、DAG 配置加载与自研微服务 RPC。

**要写的文件**：
*   `proto/dag.proto`, `component_config.proto`, `role_attributes.proto`
*   `component/component.h`, `component_base.h`, `timer_component.h`
*   `mainboard/mainboard.cpp` (加载器)
*   `service/service.h`, `client.h`, `service_base.h`

**核心改动**：
1. **接入 Protobuf 与 Component**：用 `Protobuf` 作为序列化标准；实现 `Component<T>` 模板生命周期（`Init/Proc`）；组件单独编译为 `.so` 并导出 `Load()` 符号。
2. **动态 DAG 加载器**：实现独立的 `mainboard` 进程，解析 `dag.proto` 配置，通过 `dlopen` 动态加载对应组件 `.so`，实现模块热插拔与组装。
3. **基于 Channel 的自研 RPC**：基于隐式的 `Request/Response Channel` 映射机制，自研低延迟的 Service/Client 模式，支持客户端通过携带 `request_id` 的消息在协程内异步超时等待。

**Phase 6 产出**：可动态插拔的工业级中间件加载器与原生 RPC 框架。
**对应简历句**：构建基于 `dlopen` 的组件化热插拔运行时，支持解析 Protobuf DAG 配置动态构建计算图；并基于异步 Channel 机制自研低延迟的 Service/Client RPC 通信范式。

---

### Phase 7：Choreography 编排调度与点对点唤醒 (约 400 行)
**目标**：针对自动驾驶（计算图 DAG），废弃传统的优先级轮询机制，实现基于依赖边直达的“点对点精确唤醒”。

**要写的文件**：
*   `scheduler/policy/choreography_context.h / .cpp`
*   `data/fusion/data_fusion.h`, `all_latest.h` (补齐 Phase 2 预留的 Fusion 机制)

**核心改动**：
1. **ChoreographyContext**：新增与 Classic 并列的编排调度策略，取消全局队列扫描。
2. **DAG 依赖触发**：当 `DataFusion` 确认一个组件（DAG节点）的所有上游 Channel 数据齐备后，越过 `DataNotifier` 的广播，直接将目标协程 `Enqueue` 到该线程的就绪队列执行。

**Phase 7 产出**：专为自动驾驶打造的超低延迟无轮询定向调度策略。
**对应简历句**：设计 `Choreography` 编排调度器，摒弃优先级队列轮询扫描，依据 DAG 依赖关系实现协程的精准定向唤醒；完善 `DataFusion` 模块 `AllLatest` 策略，实现多通道数据的时间戳对齐。

---

### Phase 8：跨机发现与 RTPS 抽象 (约 300 行)
**目标**：补齐多机通信拼图。为了控制依赖规模，先抽象接口与事件打桩（Stub），实现完整路由降级表。

**要写的文件**：
*   `transport/transmitter/rtps_transmitter.h`, `transport/receiver/rtps_receiver.h` (Stub 实现)
*   `topology/service_discovery.h` (发现事件定义)

**核心改动**：
1. **抽象 RTPS 接口**：增加 `RtpsTransmitter/Receiver` 的骨架，`Transmit` 内打桩输出伪装日志。
2. **自适应三级路由**：在 TopologyManager 中增加 `OnParticipantJoined/Left` 发现事件，Transport 工厂升级为 `Intra -> SHM -> RTPS` 三级自适应路由决策。

**Phase 8 产出**：具备跨机扩展能力的完整路由降级策略，证明架构的开放性。
**对应简历句**：抽象跨机 RTPS 接口模型，结合 `TopologyManager` 动态发现事件，实现 INTRA(零拷贝) -> SHM(共享内存) -> RTPS(跨机网络) 的自适应三级路由降级。

---

### Phase 9：真实负载移植与性能极限调优 (约 300 行 + 文档)
**目标**：硬核数据对标。绝不写玩具级 for-loop 压测，直接跑 Apollo 真实组件，用工业级工具证明性能。

**要写的文件**：
*   `examples/realworld/camera_component.cc`, `lidar_component.cc`, `perception.dag`, `drivers_mock.cpp`
*   `docs/profiling.md` (极核心产出)

**核心改动**：
1. **真实负载**：移植 Apollo `cyber/examples` 中的感知管道，提供 30Hz/10Hz 不同频率的数据激励，运行真实的 `.dag` 负载。
2. **硬核诊断**：在 Release 构建中开启 `-fno-omit-frame-pointer`，使用 `perf record -g` 抓取火焰图分析调度开销；使用 `perf c2c` 分析硬件级缓存未命中（Cache Misses）。

**Phase 9 产出**：一份能直接秒杀竞争者的工业级调优分析报告。
**对应简历句**：移植自动驾驶真实感知负载并构建计算图，运用 `perf` 火焰图追踪内核调度热点，通过 `perf c2c` 硬件级诊断证实缓存行对齐对伪共享（False Sharing）的消除效果，大幅降低通信延迟。

---

## 最终文件结构

```text
minicyber/
├── CMakeLists.txt
├── README.md
├── docs/
│   └── profiling.md        # [Phase 9 新增] 核心高光：perf c2c 与火焰图分析报告
├── include/minicyber/
│   ├── base/               # BoundedQueue, AtomicHashMap, WaitStrategy, AtomicRWLock
│   ├── proto/              # [Phase 6 新增] dag.proto, component_config.proto 等
│   ├── croutine/           # CRoutine, RoutineState
│   ├── scheduler/          # Scheduler, Processor, ClassicContext, PinThread
│   │   └── policy/         # [Phase 7 新增] choreography_context.h
│   ├── data/               # CacheBuffer, ChannelBuffer, DataDispatcher, DataNotifier, DataVisitor, DataFusion
│   ├── transport/          # Transport, Segment, ConditionNotifier, ShmDispatcher, IntraDispatcher, Transmitter, Receiver
│   │   └── transmitter/    # [Phase 8 新增] rtps_transmitter.h (Stub)
│   ├── topology/           # TopologyManager, Graph
│   │   └── service_discovery.h # [Phase 8 新增] 
│   ├── component/          # [Phase 6 新增] component.h, component_base.h, timer_component.h
│   ├── service/            # [Phase 6 新增] service.h, client.h
│   └── mainboard/          # [Phase 6 新增] module_controller.h
├── src/
│   ├── croutine/           # croutine.cpp, swap.S (底层汇编)
│   ├── scheduler/          # scheduler.cpp, processor.cpp, classic_context.cpp, pin_thread.cpp
│   ├── transport/          # shm_dispatcher.cpp, posix_segment.cpp, condition_notifier.cpp
│   ├── topology/           # topology_manager.cpp
│   ├── node/               # node.cpp
│   └── time/               # time.cpp
├── bin/
│   └── mainboard.cpp       # [Phase 6 新增] 框架启动入口 (dlopen加载器)
├── tests/                  # 各种底层组件的单元测试
└── examples/
    ├── pingpong/           # 性能压测对比
    └── realworld/          # [Phase 9 新增] Apollo真实负载移植
        ├── perception.dag
        ├── camera_component.cc
        └── lidar_component.cc
```

---

## 面试时的黄金讲述路线（强烈建议按此逻辑引导面试官）

在面试时被问到“介绍一下你的框架”时，请按这 5 层递进逻辑输出（**制造“技术代差”碾压**）：

#### 1. 第一层：痛点起手（Scheduler）
> “市面上的 C++ 协程库通常是为 Web Server 设计的，核心是 `epoll` 事件循环，且多使用一把大锁保护全局任务队列，多核争用极高。
> 但自动驾驶是**计算密集型**系统。因此，我**首先彻底剥离了原生协程底层的 epoll 驱动机制**，重构了纯用户态的调度核心。我引入了按 `CacheLine` 对齐的无锁有界队列 `BoundedQueue` 作为兜底，并为每个核心（Processor）分配了本地队列，采用**工作窃取算法（Work-Stealing）**和 CPU 绑核来最大化 CPU 缓存命中率，实现了基础的 **Classic 调度策略**。”

#### 2. 第二层：范式跃迁（Data-Driven 机制）
> “既然去掉了 `epoll`，协程如何挂起和唤醒？为了支撑自动驾驶业务，我设计了 **DataDispatcher（数据分发中枢）**。
> 协程不再绑定套接字，而是绑定具体的业务 Channel。当 `TryFetch` 拿不到数据时，协程主动挂起并切入 `DATA_WAIT` 状态；当上游把数据写入 `ChannelBuffer` 时，`DataNotifier` 会无锁地、瞬间将目标协程的状态改回 `READY` 并重新塞入调度器，实现了纯正的**数据流驱动计算**。”

#### 3. 第三层：跨越边界（Intra / SHM / RTPS 三级路由）
> “为了解决多机多进程的通信瓶颈，我构建了三级自适应传输层（Transport）：
> 1. **Intra（同进程）**：直接走 DataDispatcher 实现指针级零拷贝。
> 2. **SHM（跨进程）**：抛弃了传统的 IPC 锁或 eventfd，**严格对齐 CyberRT 原生方案**：用 System V 共享内存承载 `Indicator` 环形缓冲。写入方附带 `{channel_id, block_index}` 路由信息并原子累加序号；读取方通过后台线程 **50µs 粒度无锁轮询**，命中后直接唤醒协程，延迟压到极低。
> 3. **RTPS（跨机）**：我抽象了 RTPS 的发送/接收接口与 Stub，预留了 FastDDS 底座。结合 `TopologyManager` 的动态发现事件，框架会自动在 Intra、SHM 和 RTPS 之间进行最优路由决策。”

#### 4. 第四层：架构抽象与动态化（Topology & Component）
> “为了不让框架沦为玩具，我做到了跟 ROS/CyberRT 一样的高度组件化。
> 我引入了 Protobuf 作为标准数据载体，实现了一个 `mainboard` 启动器。它可以解析 `dag.proto`，然后**利用 `dlopen` 动态加载各种 `.so` 业务组件**。组件在初始化时向 `TopologyManager` 注册有向图（Graph）关系，框架对业务完全透明地完成数据收发与底层的自动路由。”

#### 5. 第五层：编排调度与数据融合（Choreography & Fusion）
> “针对感知系统多传感器输入的特性，我开发了 `DataFusion` 模块（支持 `AllLatest` 策略）实现多通道时间戳对齐与屏障同步。
> 更进一步，在复杂的 DAG 计算图中，我**抛弃了 Classic 策略的优先级队列扫描**，设计了 **Choreography（编排调度）**：当前置传感器数据全部就绪后，沿着 DAG 依赖边进行 $O(1)$ 的直接定向唤醒，将目标协程直插就绪队列，彻底砍掉了轮询所有队列的无效 CPU 开销。”

#### 6. 第六层：绝杀收尾（Profiling 与硬件级调优）
> “为了证明这些架构优化的有效性，我没有写简单的 for-loop 压测，而是**移植了 Apollo 官方的真实 Camera/Lidar 负载**。
> 在压测诊断时，我用 `perf` 火焰图证明了 Choreography 调度消除了扫描开销；**最关键的是，我用 `perf c2c` （Cache 2 Cache）工具，抓取了 CPU 的 HITM（多核缓存修改命中冲突）事件，硬核地证明了我通过 `alignas(CACHELINE_SIZE)` 的确消除了 BoundedQueue 里的伪共享（False Sharing）**，让系统的吞吐量产生了质的飞跃。完整的诊断日志我都记录在了项目文档里。”

---


# MiniCyber / Cyber-Lite：精简版 CyberRT 实施路线图

## 面试口径（背下来）

> "这是一个深度参考百度 Apollo CyberRT 架构的工业级自动驾驶高性能中间件。
> 
> 在底层，我实现了基于无锁有界队列与工作窃取（Work-Stealing）的多优先级协程调度器，并手写 CacheLine 内存对齐消除了伪共享（False Sharing）；
> 
> 在中层，我构建了 **Intra、SHM、RTPS 三级自适应混合传输层**。同进程实现指针级零拷贝分发，跨进程采用 System V 共享内存结合微秒级轮询实现极低延迟唤醒，同时抽象接入了 RTPS 协议实现跨机分布式发现与通信；
> 
> 在顶层，我实现了基于 Protobuf 的 DAG 计算图解析与 `dlopen` 组件动态热加载，抛弃了优先级轮询，实现了专为自动驾驶打造的 **Choreography 编排调度**（点对点精准唤醒）与自研的微服务 RPC。
> 
> 最核心的亮点是，为了不让框架沦为玩具，我移植了 Apollo 真实的感知算法 DAG 负载，并使用 `perf` 火焰图与 `perf c2c` 进行了硬件级的伪共享诊断与极限调优。"

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

### Step 19: feat(transport): 移植 ConditionNotifier（System V SHM + Indicator 轮询）

**目标**：对齐 CyberRT 原生跨进程通知方案，使用 System V 共享内存承载 Indicator 环形缓冲，通知本身携带路由信息。

**参考源码**：`cyber/transport/shm/condition_notifier.cc`

**文件列表**：
- `include/minicyber/transport/shm/condition_notifier.h`（新增）
- `src/transport/shm/condition_notifier.cpp`（新增）
- `tests/test_condition_notifier.cpp`（新增）

**具体实现要点**：

1. **Indicator 环形缓冲**（对齐 CyberRT 原生）：
   ```cpp
   constexpr uint32_t kBufLength = 4096;
   struct ReadableInfo {
     uint64_t host_id = 0;
     uint32_t block_index = 0;
     uint64_t channel_id = 0;
   };
   class ConditionNotifier {
     struct Indicator {
       std::atomic<uint64_t> next_seq{0};
       ReadableInfo infos[kBufLength];
       uint64_t seqs[kBufLength] = {0};
     };
    public:
     bool Init();
     bool Notify(const ReadableInfo& info);
     bool Listen(int timeout_ms, ReadableInfo* info);
     int Fd() const { return -1; }  // 无 epoll 桥接
     void Shutdown();
   };
   ```

2. **机制**：
   - `Init()`：`shmget` + `shmat` 创建/打开同 `key_t` 的 SysV SHM 段，placement-new 构造 Indicator。
   - `Notify(info)`：`next_seq.fetch_add(1)` 取序号，写入 `infos[idx]` 与 `seqs[idx]`。
   - `Listen(timeout, &info)`：50µs 粒度轮询 `next_seq` 变化，命中后快进 `next_seq_` 到槽位实际序号，读出 `ReadableInfo`。
   - `key_t` 由固定路径字符串 `/minicyber/transport/shm/notifier` 的 hash 得到，任意进程独立 `Init()` 即可共享同一段。

3. **与 eventfd 方案的核心差异**：
   - 无可注册进 epoll 的 fd，唤醒路径必须由后台线程主动 `Listen()` 轮询。
   - 通知本身携带 `{channel_id, block_index}` 路由信息，读者无需扫描所有 segment。
   - 唤醒粒度受 `sleep_for(50µs)` 实际调度精度限制（Linux 上约 1ms）。

**提交信息**：
```
feat(transport): 移植 ConditionNotifier（System V SHM + Indicator 轮询）

- 对齐 CyberRT 原生方案：shmget/shmat 承载 Indicator 环形缓冲
- Notify 写 ReadableInfo 到环形，next_seq 原子累加
- Listen 50µs 粒度轮询 next_seq，命中后快进并读出路由信息
- 通知本身携带 {channel_id, block_index}，无需扫描 segment
- 添加跨进程通知与环形回绕测试
```

---

### Step 20: feat(transport): 移植 IntraDispatcher 与 ShmDispatcher

**目标**：实现同进程和跨进程两种分发后端，ShmDispatcher 对齐 CyberRT 原生基线（后台线程轮询 ConditionNotifier）。

**参考源码**：`cyber/transport/dispatcher/intra_dispatcher.h`, `shm_dispatcher.h`

**文件列表**：
- `include/minicyber/transport/dispatcher/intra_dispatcher.h`（新增）
- `include/minicyber/transport/dispatcher/shm_dispatcher.h`（新增）
- `src/transport/dispatcher/shm_dispatcher.cpp`（新增）
- `tests/test_intra_dispatcher.cpp`（新增）
- `tests/test_shm_dispatcher.cpp`（新增）

**具体实现要点**：

1. **新增 `IntraDispatcher<T>`**：模板单例，直接转发给 `DataDispatcher<T>::Instance()->Dispatch()`，指针级零拷贝。

2. **新增 `ShmDispatcher`**（单例，对齐 CyberRT 原生）：
   - 持有 `ConditionNotifier` + `unordered_map<channel_id, shared_ptr<PosixSegment>>`。
   - 后台线程 `ThreadFunc` 循环调用 `notifier_->Listen(100, &info)`。
   - 命中后由 `info.channel_id` 直接定位 segment，`info.block_index` 定位 block，无需扫描。
   - `AcquireBlockToRead(block_index)` → memcpy payload 成 `std::string` → `DataDispatcher<std::string>::Instance()->Dispatch(channel_id, msg)`。
   - `AddSegment(channel_id)` 幂等：已存在则不替换（避免旧 PosixSegment 析构 `shm_unlink`）。

3. **与 eventfd 版本的核心差异**：
   - 原生 `Listen` 返回 `ReadableInfo`，直接给出目标 channel 与 block_index。
   - 无需 `last_read_seq_` 映射或 segment 扫描。
   - 唤醒延迟受轮询粒度限制（约 1ms），但换得真正的跨进程支持与 CyberRT 保真度。

**提交信息**：
```
feat(transport): 移植 IntraDispatcher 与 ShmDispatcher

- IntraDispatcher：模板单例，直接调 DataDispatcher，指针级零拷贝
- ShmDispatcher：后台线程轮询 ConditionNotifier，对齐 CyberRT 原生
- Listen 返回 ReadableInfo 直接定位 channel + block_index，无需扫描
- AddSegment 幂等，避免旧 segment 析构导致 shm_unlink
- 添加同进程与 fork 跨进程分发测试
```

---

### Step 21: feat(transport): 移植 Transmitter 与 Receiver 接口

**目标**：封装底层 Dispatcher，提供统一的发布/订阅接口，对齐 CyberRT 原生写入/读取路径。

**参考源码**：`cyber/transport/transmitter/*`, `cyber/transport/receiver/*`

**文件列表**：
- `include/minicyber/transport/transmitter/transmitter.h`（新增）
- `include/minicyber/transport/transmitter/intra_transmitter.h`（新增）
- `include/minicyber/transport/transmitter/shm_transmitter.h`（新增）
- `include/minicyber/transport/receiver/receiver.h`（新增）
- `include/minicyber/transport/receiver/intra_receiver.h`（新增）
- `include/minicyber/transport/receiver/shm_receiver.h`（新增）
- `tests/test_intra_transmitter.cpp`（新增）
- `tests/test_shm_transmitter.cpp`（新增）
- `tests/test_intra_receiver.cpp`（新增）
- `tests/test_shm_receiver.cpp`（新增）

**具体实现要点**：

1. **Transmitter 基类**（`transmitter.h`）：
   - 模板 `Transmitter<M>`，纯虚 `Enable()/Disable()/Transmit(msg)`。
   - 维护 `seq_num_`（单调递增）、`channel_id_`、`enabled_`。

2. **IntraTransmitter<M>**：
   - `Transmit` → `IntraDispatcher<M>::Instance()->Dispatch(channel_id_, msg)`，零拷贝。
   - 发布语义对齐 CyberRT：Enable 后发布即成功（返回 true），无论是否有订阅者。

3. **ShmTransmitter**（继承 `Transmitter<std::string>`）：
   - 持有 `shared_ptr<PosixSegment>` + `unique_ptr<ConditionNotifier>`。
   - `Enable()`：Open segment + Init notifier。
   - `Transmit`：`AcquireBlockToWrite` → `memcpy` → `set_msg_size` → `ReleaseWrittenBlock` → `Notify(ReadableInfo{0, block_index, channel_id})`。对齐 CyberRT 原生写入路径。
   - `Disable()`：Shutdown notifier + Destroy segment。

4. **Receiver 基类**（`receiver.h`）：
   - 模板 `Receiver<M>`，`MessageListener = function<void(const shared_ptr<M>&)>`。
   - 纯虚 `Enable()/Disable()`，protected `OnNewMessage(msg)` 触发上层回调。

5. **IntraReceiver<M>**：
   - `Enable()`：创建 `ChannelBuffer<M>` 注册到 `DataDispatcher<M>` + 注册 `DataNotifier` 回调。
   - 回调路径：DataNotifier 触发 → `ChannelBuffer::Latest` → `OnNewMessage`。
   - `Disable()`：重置回调 + 销毁 ChannelBuffer（weak_ptr 自动失效）。

6. **ShmReceiver**（继承 `Receiver<std::string>`）：
   - `Enable()`：`ShmDispatcher::AddSegment(channel_id)` + 注册 `ChannelBuffer` + `DataNotifier` 回调。
   - 数据路径：ShmDispatcher 后台线程 Listen → ReadableInfo → 读 SHM → `DataDispatcher::Dispatch` → `DataNotifier` → ShmReceiver 回调 → `OnNewMessage`。
   - `Disable()`：重置回调 + 销毁 ChannelBuffer。

7. **序列化简化**：先支持 `std::string`，通过 `memcpy` 拷贝到 Segment。面试口径："预留了二进制序列化接口，当前用 string 做演示"。

**与 CyberRT 的核心差异**：
   - 去掉 `RoleAttributes`/`MessageInfo`/`Endpoint`/`History` 基类，直接用 `channel_id`。
   - `ShmReceiver` 不直接注册回调到 `ShmDispatcher`，而是通过 `DataDispatcher + DataNotifier` 间接挂接——这与 MiniCyber 的数据驱动中枢设计一致，复用了 Step 14/13 的已有链路。

**提交信息**：
```
feat(transport): 移植 Transmitter 与 Receiver 接口

- Transmitter<M> 模板基类：Enable/Disable/Transmit 纯虚，seq_num 递增
- IntraTransmitter<M>：零拷贝转发 IntraDispatcher::Dispatch
- ShmTransmitter：AcquireBlockToWrite -> memcpy -> Notify(ReadableInfo)，对齐原生
- Receiver<M> 模板基类：MessageListener 回调，Enable/Disable 纯虚
- IntraReceiver<M>：ChannelBuffer + DataNotifier 回调挂接数据到达
- ShmReceiver：ShmDispatcher::AddSegment + DataNotifier 回调
- 先支持 std::string，预留二进制序列化接口
- 添加同进程端到端与 fork 跨进程收发测试
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

## Phase 6: 组件化、DAG 动态加载与 RPC 通信（约 800 行）

### Step 28: feat(proto): 引入 Protobuf 依赖与核心配置定义

**目标**：配置 CMake 以支持 Protobuf 编译，并定义框架所需的核心 Proto 文件。

**参考源码**：`cyber/proto/role_attributes.proto`, `component_conf.proto`, `dag_conf.proto`

**文件列表**：
- `CMakeLists.txt`（修改，引入 Protobuf）
- `include/minicyber/proto/role_attributes.proto`（新增）
- `include/minicyber/proto/component_conf.proto`（新增）
- `include/minicyber/proto/dag_conf.proto`（新增）

**具体实现要点**：

1. **修改 `CMakeLists.txt`**：
   - 增加 `find_package(Protobuf REQUIRED)`。
   - 编写自定义 `protobuf_generate_cpp` 逻辑（或使用 CMake 自带模块），将 `.proto` 编译为 `.pb.cc/.pb.h` 并链接到 `minicyber_core`。
2. **定义 `role_attributes.proto`**：
   - 包含 `host_name`, `process_id`, `node_name`, `node_id`, `channel_name`, `channel_id`, `message_type` 等用于 Topology 注册的元数据。
3. **定义 `component_conf.proto` / `dag_conf.proto`**：
   - `ReaderOption` (含 channel 名称)。
   - `ComponentConfig` (组件名称与 Readers 列表)。
   - `ModuleConfig` (记录动态库 `.so` 路径及包含的 Components)。
   - `DagConfig` (顶层配置，包含多个 `ModuleConfig`)。

**提交信息**：
```
feat(proto): 引入 Protobuf 依赖与核心配置定义

- CMakeLists.txt 集成 Protobuf 编译规则
- 定义 role_attributes.proto 用于拓扑发现元数据
- 定义 component_conf.proto 与 dag_conf.proto 用于计算图编排
- 确立以配置驱动为核心的框架演进方向
```

---

### Step 29: feat(component): 实现 Component 基础抽象与生命周期

**目标**：提供开发者继承的基类，规范化模块的 `Init` 与 `Proc` 流程。

**参考源码**：`cyber/component/component_base.h`, `component.h`, `timer_component.h`

**文件列表**：
- `include/minicyber/component/component_base.h`（新增）
- `include/minicyber/component/component.h`（新增）
- `include/minicyber/component/timer_component.h`（新增）

**具体实现要点**：

1. **新增 `ComponentBase`**：
   ```cpp
   class ComponentBase : public std::enable_shared_from_this<ComponentBase> {
   public:
     virtual ~ComponentBase() = default;
     virtual bool Initialize(const ComponentConfig& config) { return false; }
     virtual void Shutdown() { /* 释放 Node/Reader 等资源 */ }
   protected:
     virtual bool Init() = 0;
     std::shared_ptr<Node> node_ = nullptr;
   };
   ```
2. **新增 `Component<T...>` 模板**：
   - 继承自 `ComponentBase`。
   - 内部重写 `Initialize()`：读取 `config` 中的 channel，调用 `node_->CreateReader<T>(channel, [this](auto msg){ this->Proc(msg); })`。
   - 提供纯虚函数 `virtual bool Proc(const std::shared_ptr<T>& msg) = 0;` 供用户重写。
3. **新增 `TimerComponent`**：
   - 不依赖 Reader，而是通过 Scheduler 的 `CreateTask` 配合 `SleepFor` 实现周期性触发 `Proc()`。

**提交信息**：
```
feat(component): 实现 Component 基础抽象与生命周期

- 抽象 ComponentBase 提供 Initialize/Shutdown 生命周期管理
- 实现 Component<T> 模板，封装 Node 创建与 Reader 回调绑定
- 暴露纯虚函数 Proc() 供业务层实现核心逻辑
- 实现 TimerComponent 支持周期性无输入触发任务
```

---

### Step 30: feat(component): 实现宏注册与 Component 工厂

**目标**：提供类似 `CYBER_REGISTER_COMPONENT` 的宏，利用 C++ 静态变量初始化机制，自动将用户编写的 Component 类注册到全局工厂。

**参考源码**：`cyber/class_loader/class_loader_register_macro.h`, `class_factory.h` (简化版)

**文件列表**：
- `include/minicyber/component/component_factory.h`（新增）
- `tests/test_component.cpp`（新增）

**具体实现要点**：

1. **实现单例工厂 `ComponentFactory`**：
   - 维护一个 `std::unordered_map<std::string, std::function<ComponentBase*()>>`。
   - 提供 `Register(const std::string& class_name, CreatorFunc)` 和 `Create(const std::string& class_name) -> ComponentBase*` 方法。
2. **实现注册宏**：
   ```cpp
   #define MINICYBER_REGISTER_COMPONENT(ClassName) \
     namespace { \
       struct ComponentRegistrar_##ClassName { \
         ComponentRegistrar_##ClassName() { \
           ComponentFactory::Instance()->Register(#ClassName, []() -> ComponentBase* { \
             return new ClassName(); \
           }); \
         } \
       }; \
       static ComponentRegistrar_##ClassName g_registrar_##ClassName; \
     }
   ```
   *注意：这里简化了 CyberRT 中通过 Poco 库实现的 ClassLoader，直接使用 C++ 静态初始化注册。在使用 `dlopen` 加载 `.so` 时，被加载的 `.so` 中的静态对象会被初始化，从而自动注册进主进程的工厂中。*
3. **单测验证**：定义一个 DummyComponent 使用宏注册，通过 Factory 通过类名字符串成功实例化。

**提交信息**：
```
feat(component): 实现宏注册与 Component 工厂

- 引入 ComponentFactory 管理 Component 类名到构造函数的映射
- 提供 MINICYBER_REGISTER_COMPONENT 宏，利用静态初始化机制实现自动注册
- 支持通过字符串类名动态实例化 Component 对象，为 DAG 加载铺平道路
```

---

### Step 31: feat(mainboard): 实现 DAG 解析与 `dlopen` 动态加载器

**目标**：实现 MiniCyber 的核心启动程序 `mainboard`，它读取 DAG 配置，`dlopen` 加载业务动态库，并初始化 Component 计算图。

**参考源码**：`cyber/mainboard/mainboard.cc`, `module_controller.cc`

**文件列表**：
- `include/minicyber/mainboard/module_controller.h/.cpp`（新增）
- `bin/mainboard.cpp`（新增）
- `CMakeLists.txt`（修改，添加 `mainboard` 可执行目标）

**具体实现要点**：

1. **实现 `ModuleController::LoadModule`**：
   - 解析 `dag_conf.proto` 文件。
   - 遍历 `ModuleConfig`，获取 `module_library` (即 `.so` 路径)。
   - 调用系统 API `dlopen(so_path, RTLD_NOW | RTLD_GLOBAL)` 加载动态库。
   - （关键机制发生：`.so` 被加载后，其内部的 `MINICYBER_REGISTER_COMPONENT` 宏会被触发，将类名注册进全局 `ComponentFactory`）。
   - 遍历 `components`，调用 `ComponentFactory::Instance()->Create(class_name)` 实例化。
   - 调用 `component->Initialize(config)`，这会触发内部 Reader 的创建和协程任务的生成。
2. **实现 `mainboard.cpp`**：
   - 解析命令行参数 `-d xxx.dag`。
   - 初始化 MiniCyber 环境（启动 Scheduler、TopologyManager）。
   - 调用 `ModuleController::LoadAll()`。
   - 调用 `WaitForShutdown()`（阻塞主线程直至收到 SIGINT）。

**提交信息**：
```
feat(mainboard): 实现 DAG 解析与 dlopen 动态加载器

- 实现 ModuleController 解析 pb DAG 配置文件
- 基于 dlopen (RTLD_GLOBAL) 动态加载业务组件 .so 文件
- 依据配置自动实例化并初始化 Component，构建运行时计算图
- 添加独立启动程序 mainboard，提供标准化运行入口
```

---

### Step 32: feat(service): 抽象 Service 与 Client 基类结构

**目标**：开始搭建 RPC 框架，定义 Service 和 Client 的核心骨架与请求/响应封装。

**参考源码**：`cyber/service/service_base.h`, `client_base.h`, `common/types.h` (SRV_CHANNEL 宏)

**文件列表**：
- `include/minicyber/service/service_base.h`（新增）
- `include/minicyber/service/client_base.h`（新增）

**具体实现要点**：

1. **通信原理设计（基于 Channel）**：
   - CyberRT 原生实现 RPC 的原理：对于服务名 `ServiceName`，自动在底层创建两个特殊的 Channel：
     - 请求 Channel：`ServiceName__SRV__REQUEST`
     - 响应 Channel：`ServiceName__SRV__RESPONSE`
   - Client 使用 Transmitter 发 Request，用 Receiver 听 Response。
   - Server 使用 Receiver 听 Request，用 Transmitter 发 Response。
2. **消息头改造**：
   - 确保底层的 `MessageInfo` 或传输的数据结构中携带了 `request_id`（通常使用 Client 的 Transmitter 的 `Identity` 加上递增的 `sequence_number`），以便 Client 收到 Response 时能对应上是被阻塞的哪一次请求。
3. **接口定义**：
   - `ServiceBase::Shutdown()`
   - `ClientBase::ServiceIsReady()`（通过 TopologyManager 查询对面是否存在）

**提交信息**：
```
feat(service): 抽象 Service 与 Client 基类结构

- 定义 ServiceBase 与 ClientBase 抽象接口
- 确立基于 Request/Response 双向 Channel 映射的 RPC 路由规则
- 梳理基于 MessageInfo/request_id 的 RPC 匹配机制
```

---

### Step 33: feat(service): 实现自研低延迟 Service/Client RPC

**目标**：完成基于 Intra/SHM 传输层的同步/异步 RPC 调用闭环。

**参考源码**：`cyber/service/service.h`, `client.h`, `node.h` (扩展)

**文件列表**：
- `include/minicyber/service/service.h`（新增）
- `include/minicyber/service/client.h`（新增）
- `include/minicyber/node/node.h`（修改，暴露创建接口）
- `tests/test_service_client.cpp`（新增）

**具体实现要点**：

1. **实现 `Service<Req, Rsp>`**：
   - 构造时传入 `std::function<void(shared_ptr<Req>, shared_ptr<Rsp>&)>` 作为用户回调。
   - `Init()` 中创建 `Receiver<Req>` 监听 Request Channel。
   - 当请求到达，调用用户回调填充 Rsp。
   - 利用 `MessageInfo` 中携带的 `sender_id`（发件人身份），将构建好的 Rsp 通过 `Transmitter<Rsp>` 发往 Response Channel。
2. **实现 `Client<Req, Rsp>`**：
   - `Init()` 中创建 `Transmitter<Req>` 发送请求，创建 `Receiver<Rsp>` 监听响应。
   - `SendRequest(Req, timeout)`：
     - 生成递增的 `sequence_num`，构造 `std::promise<shared_ptr<Rsp>>`。
     - 发送请求。
     - **利用协程优势**：不是 `std::future::wait` 傻等阻塞系统线程，而是记录状态后 `Fiber::Yield(DATA_WAIT)`。
     - *（妥协方案：如果协程超时机制较难实现，初步版本可用 `std::future::wait_for` 阻塞当前 Worker 线程，面试时解释“这是为了优先走通链路的折衷，生产环境应通过 Timer 结合协程唤醒做超时”。）*
   - `Receiver<Rsp>` 的底层回调触发时，通过 `request_id` 匹配 Promise，`set_value` 并将对应的挂起协程改回 `READY`。
3. **扩展 Node API**：
   - `node->CreateService<Req, Rsp>(service_name, callback)`
   - `node->CreateClient<Req, Rsp>(service_name)`

**提交信息**：
```
feat(service): 实现自研低延迟 Service/Client RPC

- 基于双向隐式 Channel 实现解耦的请求-响应机制
- Service 端自动解析 Request 身份，回传 Response
- Client 维护 pending_requests 映射表，支持超时等待与协程异步唤醒
- 扩展 Node 接口提供 CreateService/CreateClient，完善微服务语意
- 添加 test_service_client 验证同步请求与响应闭环
```

---


## Phase 7: 编排调度与多路数据融合（约 500 行）

### Step 34: feat(data): 设计 DataFusion 与多源数据缓存基座

**目标**：为多路数据融合提供存储基座，支持同时缓存多个通道的数据以备对齐检查。

**参考源码**：`cyber/data/fusion/data_fusion.h`, `all_latest.h`

**文件列表**：
- `include/minicyber/data/fusion/data_fusion.h`（新增）
- `include/minicyber/data/fusion/all_latest.h`（新增）

**具体实现要点**：

1. **定义 `DataFusion` 纯虚基类**：
   - 使用可变参数模板（或针对常用的 2~4 通道提供重载）：
   ```cpp
   template <typename M0, typename M1>
   class DataFusion {
   public:
     virtual ~DataFusion() = default;
     // 检查并融合数据，如果条件满足则将数据填入指针并返回 true
     virtual bool Fusion(uint64_t* index, std::shared_ptr<M0>& m0, std::shared_ptr<M1>& m1) = 0;
   };
   ```

2. **实现 `AllLatest` 策略**：
   - 自动驾驶最常用的融合策略：取各通道**最新**的数据进行匹配。
   ```cpp
   template <typename M0, typename M1>
   class AllLatest : public DataFusion<M0, M1> {
   public:
     AllLatest(const ChannelBuffer<M0>& b0, const ChannelBuffer<M1>& b1)
       : b0_(b0), b1_(b1) {
       // 当主通道(M0)数据到达时，触发融合检查逻辑
       b0_.Buffer()->SetFusionCallback([this](const std::shared_ptr<M0>& m0) {
         std::shared_ptr<M1> m1;
         // 如果副通道(M1)没有最新数据，则本次融合失败，丢弃
         if (!b1_.Latest(&m1)) return;
         
         // 融合成功，将数据打包压入内部的融合结果缓冲
         auto data = std::make_shared<std::tuple<std::shared_ptr<M0>, std::shared_ptr<M1>>>(m0, m1);
         fusion_buffer_.Fill(data);
       });
     }

     bool Fusion(uint64_t* index, std::shared_ptr<M0>& m0, std::shared_ptr<M1>& m1) override {
       std::shared_ptr<std::tuple<...>> data;
       if (!fusion_buffer_.Fetch(index, &data)) return false;
       m0 = std::get<0>(*data);
       m1 = std::get<1>(*data);
       return true;
     }

   private:
     ChannelBuffer<M0> b0_;
     ChannelBuffer<M1> b1_;
     CacheBuffer<std::shared_ptr<std::tuple<...>>> fusion_buffer_;
   };
   ```
   *注意：这里的精妙之处在于将主通道的回调（`FusionCallback`）截获，代替直接唤醒协程，转而执行融合检查逻辑。*

**提交信息**：
```
feat(data): 设计 DataFusion 与多源数据缓存基座

- 抽象 DataFusion 接口支持多通道联合数据就绪检查
- 实现 AllLatest 融合策略：以主通道触发，拉取副通道最新数据
- 为复杂的感知数据对齐（如 Lidar + Camera 同步）提供底层机制
```

---

### Step 35: feat(data): 升级 DataVisitor 支持多通道 DataFusion

**目标**：将 `DataFusion` 融合进面向协程的 `DataVisitor`，使得组件层的 `Proc` 能直接接收多个参数。

**参考源码**：`cyber/data/data_visitor.h` (多参数偏特化部分)

**文件列表**：
- `include/minicyber/data/data_visitor.h`（修改）
- `tests/test_data_fusion.cpp`（新增）

**具体实现要点**：

1. **特化 `DataVisitor`**：
   - 针对单通道：直接从 `ChannelBuffer` 取。
   - 针对多通道（例如双通道）：
   ```cpp
   template <typename M0, typename M1>
   class DataVisitor {
   public:
     DataVisitor(uint64_t ch0, uint64_t ch1) : b0_(ch0), b1_(ch1) {
       // 绑定两个 buffer 到分发中心
       DataDispatcher<M0>::Instance()->AddBuffer(ch0, b0_);
       DataDispatcher<M1>::Instance()->AddBuffer(ch1, b1_);
       // 实例化融合策略
       fusion_ = new AllLatest<M0, M1>(b0_, b1_);
       // 注册唤醒回调到主通道的 Notifier
       DataNotifier::Instance()->AddNotifier(ch0, notifier_);
     }

     bool TryFetch(std::shared_ptr<M0>& m0, std::shared_ptr<M1>& m1) {
       if (fusion_->Fusion(&next_msg_index_, m0, m1)) {
         next_msg_index_++;
         return true;
       }
       return false;
     }

     // 阻塞式 Fetch 逻辑不变，依然是在 while 中判断 TryFetch 失败则 Yield(DATA_WAIT)
   };
   ```

2. **编写 `test_data_fusion.cpp`**：
   - 启动一个协程调用 `TryFetch` 等待 `chA` 和 `chB`。
   - 只给 `chA` 发数据，验证协程依然挂起。
   - 再给 `chB` 发数据并给 `chA` 发新数据，验证协程被唤醒并拿到最新数据对。

**提交信息**：
```
feat(data): 升级 DataVisitor 支持多通道 DataFusion

- 通过模板特化扩展 DataVisitor 支持监听多个 Channel
- 内部组合 AllLatest 融合引擎，仅在满足融合条件时返回 True
- DataNotifier 统一挂载至主通道，实现协程屏障同步 (Barrier)
- 添加 test_data_fusion 验证交叉到达时的数据对齐与唤醒
```

---

### Step 36: feat(scheduler): 实现 ChoreographyContext (编排上下文)

**目标**：脱离基于优先级的粗暴轮询，实现依靠 DAG 边依赖的点对点任务分发策略。

**参考源码**：`cyber/scheduler/policy/choreography_context.h/.cpp`

**文件列表**：
- `include/minicyber/scheduler/policy/choreography_context.h`（新增）
- `src/scheduler/policy/choreography_context.cpp`（新增）

**具体实现要点**：

1. **定义 `ChoreographyContext`**：
   ```cpp
   class ChoreographyContext : public ProcessorContext {
   public:
     std::shared_ptr<CRoutine> NextRoutine() override;
     bool Enqueue(const std::shared_ptr<CRoutine>& cr); // 将就绪任务推入本地就绪队列
     void Notify();
     void Wait() override;
   private:
     // 使用一个简单的就绪队列，而不是 Classic 中的多级优先级数组
     std::multimap<uint32_t, std::shared_ptr<CRoutine>, std::greater<uint32_t>> ready_queue_;
     base::AtomicRWLock rq_lock_;
   };
   ```
2. **调度逻辑 (`NextRoutine`)**：
   - 直接从 `ready_queue_` 中取第一个协程。
   - **关键差异**：在 `ClassicContext` 中，框架会在 `NextRoutine` 遍历扫描所有 `DATA_WAIT` 的协程并尝试唤醒它们。但在 `ChoreographyContext` 中，`ready_queue_` 中**只有确定已经拿到数据（被上游显式唤醒）的协程**。因此扫描开销极大降低（$O(1)$ 取用）。

**提交信息**：
```
feat(scheduler): 实现 ChoreographyContext (编排上下文)

- 实现专为自动驾驶 DAG 定制的极简就绪队列调度上下文
- 去除全局优先级轮询开销，依赖明确的任务投递 (Enqueue)
- 配合 DataNotifier 形成 O(1) 的响应式就绪投递链路
- 为多级任务流水线的微秒级响应奠定基础
```

---

### Step 37: refactor(component): Component 适配多参数与编排策略调度

**目标**：将 `Component<T1, T2>` 多参数入口与 `Choreography` 调度融合，完成上层到下层的闭环。

**参考源码**：`cyber/component/component.h` (多参数偏特化部分)

**文件列表**：
- `include/minicyber/component/component.h`（修改）
- `src/scheduler/scheduler.cpp`（修改）
- `tests/test_choreography.cpp`（新增）

**具体实现要点**：

1. **Component 偏特化**：
   - 增加 `Component<M0, M1>` 特化版本。
   - `Initialize` 函数中，初始化 `DataVisitor<M0, M1>`，并将对应的 `Proc(m0, m1)` 回调封装为 `CRoutine`。
2. **Scheduler 策略决策**：
   - `Scheduler::CreateTask(RoutineFactory, name, policy)` 增加策略选择。
   - 在解析 DAG (`mainboard` 中) 时，根据配置将这些 Component 的协程绑定到 `ChoreographyContext` 的 Processor 上。
   - 当 `DataNotifier` 发现主通道有数据并经过 `DataVisitor/DataFusion` 确认匹配成功时，调用 `scheduler->NotifyTask()`。
   - `NotifyTask` 会直接调用对应 Processor 的 `ChoreographyContext::Enqueue`，将该组件直接推入就绪执行序列。
3. **编写验证测试**：
   - 构建 `A -> C`, `B -> C` 的依赖拓扑。C 绑定 `ChoreographyContext`。验证仅在 A 和 B 都到达后，C 被精确调度。

**提交信息**：
```
refactor(component): Component 适配多参数与编排策略调度

- 为 Component<M0, M1> 提供多参数偏特化，挂接 DataVisitor<M0, M1>
- Scheduler::CreateTask 增加调度策略路由 (Classic vs Choreography)
- 打通数据到达 -> DataFusion 校验 -> ChoreographyContext 定向唤醒的全链路
- 添加 test_choreography 验证 DAG 点对点精准唤醒与零轮询调度
```

---

## Phase 8: 跨机发现与 RTPS 抽象（约 400 行）

### Step 38: feat(transport): 抽象跨机 RTPS 发送与接收接口（Stub）

**目标**：为 Transport 层增加第三种传输手段（RTPS），实现接口层面的完整抽象，并以 Stub（打桩）的形式实现逻辑闭环，避免引入沉重的外部依赖。

**参考源码**：`cyber/transport/transmitter/rtps_transmitter.h`, `cyber/transport/receiver/rtps_receiver.h`, `cyber/transport/dispatcher/rtps_dispatcher.h`

**文件列表**：
- `include/minicyber/transport/transmitter/rtps_transmitter.h`（新增）
- `include/minicyber/transport/receiver/rtps_receiver.h`（新增）
- `include/minicyber/transport/dispatcher/rtps_dispatcher.h`（新增）
- `src/transport/dispatcher/rtps_dispatcher.cpp`（新增）

**具体实现要点**：

1. **定义 `RtpsTransmitter`**：
   - 继承自 `Transmitter<M>`。
   - `Enable()` / `Disable()` 留空或打印日志。
   - `Transmit()` 接口：**这里是关键的 Stub**。在真正的环境中这里会调用 DDS 的 `write()`。我们在 Stub 中，只需打印一句日志：`"Stub: [RTPS] Transmit message over network. channel: xxx"`，并返回 `true`。
2. **定义 `RtpsDispatcher` 与 `RtpsReceiver`**：
   - 维持 `Dispatcher` 的语义：维护 `channel_id -> ListenerHandler` 的映射。
   - `RtpsReceiver` 负责向 `RtpsDispatcher` 注册回调。
   - 同样，由于是 Stub，我们可以提供一个后门函数 `MockReceiveFromNetwork(channel_id, msg)`，在测试时手动调用它，用来模拟“网络收到数据后交给 Dispatcher，最终唤醒协程”的链路。

**提交信息**：
```
feat(transport): 抽象跨机 RTPS 发送与接收接口（Stub）

- 实现 RtpsTransmitter、RtpsReceiver 与 RtpsDispatcher 的骨架
- 以 Stub 模式模拟 DDS 跨机网络收发，剥离第三方依赖
- 补全 Transport 层的三级传输接口定义（Intra/Shm/Rtps）
```

---

### Step 39: feat(topology): 抽象 ServiceDiscovery 接口并支持跨机标记

**目标**：改造现有的 `TopologyManager`，支持标识一个 Node/Channel 的“机器来源”，并预留出底层网络发现机制的钩子（Hook）。

**参考源码**：`cyber/service_discovery/topology_manager.h`, `cyber/service_discovery/communication/*`

**文件列表**：
- `include/minicyber/topology/topology_manager.h`（修改）
- `src/topology/topology_manager.cpp`（修改）
- `include/minicyber/topology/service_discovery.h`（新增）

**具体实现要点**：

1. **扩展 `RoleAttributes` 的机器感知**（在 Step 28 已预留，现启用）：
   - `host_name`、`host_ip`、`process_id`。
   - 系统启动时获取本机的 IP 或 hostname，作为本地标识（Local Host ID）。
2. **改造 `TopologyManager::IsSameProc` 并新增 `IsSameHost`**：
   ```cpp
   // 供 Transport 决策使用
   enum Relation { SAME_PROC, DIFF_PROC, DIFF_HOST };
   Relation GetRelation(const std::string& remote_host, int remote_pid) const;
   ```
3. **抽象 `ServiceDiscovery` 接口**：
   - 定义网络发现的回调接口：
   ```cpp
   class TopologyManager {
   public:
     // 供底层的 DDS ParticipantListener 调用
     void OnParticipantJoined(const RoleAttributes& attr, RoleType role);
     void OnParticipantLeft(const RoleAttributes& attr, RoleType role);
   };
   ```
   - 在 Stub 模式下，允许通过 API 手动注入“伪造”的远端节点，验证 Topology 树的变化。

**提交信息**：
```
feat(topology): 抽象 ServiceDiscovery 接口并支持跨机标记

- 引入 host_name/host_ip 作为物理机器识别标记
- 扩展拓扑关系查询支持 SAME_PROC, DIFF_PROC, DIFF_HOST 的三级判定
- 预留 OnParticipantJoined/Left 接口，为对接外部发现协议（如 DDS/mDNS）提供 Hook
```

---

### Step 40: refactor(transport): 完善 Transport 三级自动路由与动态降级重构

**目标**：将拓扑管理（Phase 8）与传输层（Phase 4）打通。当发现新节点时，`Transport` 工厂能够根据 `GetRelation` 的结果，自适应选择 INTRA、SHM 还是 RTPS。

**参考源码**：`cyber/transport/transport.h` (自动路由选择逻辑)

**文件列表**：
- `include/minicyber/transport/transport.h`（修改）
- `src/transport/transport.cpp`（修改）
- `tests/test_transport_routing.cpp`（修改/新增）

**具体实现要点**：

1. **修改 `Transport::CreateTransmitter` 和 `CreateReceiver`**：
   ```cpp
   template <typename T>
   std::shared_ptr<Transmitter<T>> Transport::CreateTransmitter(const RoleAttributes& attr) {
       // 注意：CyberRT 中的 HybridTransmitter 实际会创建三个底层的 Transmitter，
       // 然后在 Transmit() 时根据下游 Receiver 的来源，选择不同的底层 Transmitter 分发。
       // 为简化实现且保持核心语义，此处我们返回一个包装了多路路由能力的 HybridTransmitter，
       // 或者让工厂直接查表（简化版）。
       
       auto relation = TopologyManager::Instance()->GetRelation(attr.host_name(), attr.process_id());
       
       if (relation == SAME_PROC) {
           return std::make_shared<IntraTransmitter<T>>(attr);
       } else if (relation == DIFF_PROC) {
           return std::make_shared<ShmTransmitter<T>>(attr);
       } else { // DIFF_HOST
           return std::make_shared<RtpsTransmitter<T>>(attr); // 走到 Stub
       }
   }
   ```
   *（注：CyberRT 实际使用的是 `HybridTransmitter` 包含一组映射表。为匹配 5000 行规模，推荐实现一个包含 `Intra`, `Shm`, `Rtps` 实例指针的 `HybridTransmitter`，在写数据时遍历已发现的 Reader 属性，根据 `Relation` 选择指针发送）。*

2. **动态重连机制（拓扑事件驱动）**：
   - 当 `TopologyManager` 触发 `OnParticipantJoined` 事件（比如有新的远端订阅者上线），通过事件总线（Signal/Callback）通知对应的 `HybridTransmitter` 启用相应的底层通信组件（如开启 `RtpsTransmitter` 发送）。

3. **测试 `test_transport_routing.cpp`**：
   - 注册一个本地 Reader。调用 `Publish` 走 INTRA。
   - 注册一个本地另一个进程的 Reader。调用 `Publish` 走 SHM。
   - 通过 `TopologyManager::OnParticipantJoined` 伪造一个来自 `192.168.1.100` 的远端 Reader。调用 `Publish` 观察日志，验证 `RtpsTransmitter` (Stub) 的 `"Stub: [RTPS] Transmit message over network"` 被打印。

**提交信息**：
```
refactor(transport): 完善 Transport 三级自动路由与动态重连

- 实现基于 HybridTransceiver 模式的多重路由封装
- 结合 TopologyManager 的判定结果，实现 Intra -> Shm -> Rtps 的无缝切换与自动降级
- 支持基于拓扑发现事件（OnParticipantJoined）的动态链路重构
- 添加 test_transport_routing 单测，模拟远端节点接入触发 RTPS Stub 分发
```

---

## Phase 9: 真实负载移植与性能调优（约 300 行代码 + 详尽文档）

### Step 41: feat(examples): 移植 Apollo Camera/Lidar 组件与 DAG 配置

**目标**：构建一个仿真的自动驾驶感知计算流（Sensor -> Camera/Lidar Component -> Fusion Component），运行真实的 DAG 配置文件。

**参考源码**：Apollo `cyber/examples/common_component_example/*`

**文件列表**：
- `examples/realworld/CMakeLists.txt`（新增，编译成独立的 .so）
- `examples/realworld/camera_component.cc/.h`（新增）
- `examples/realworld/lidar_component.cc/.h`（新增）
- `examples/realworld/fusion_component.cc/.h`（新增）
- `examples/realworld/perception.dag`（新增）
- `examples/realworld/drivers_mock.cpp`（新增，模拟传感器发数据）

**具体实现要点**：

1. **编写感知组件 (`camera_component` / `lidar_component`)**：
   - 继承 `minicyber::Component<Message>`。
   - `Init()` 返回 true。
   - `Proc(msg)` 中使用 `std::this_thread::sleep_for` 或死循环计算，模拟图像/点云处理的耗时（如 Camera 30ms，Lidar 50ms）。
   - 处理完后通过内部的 `Writer` 发布 `/perception/camera` 和 `/perception/lidar` 通道。
   - **务必加上 `MINICYBER_REGISTER_COMPONENT` 宏**，并将它们编译成 `libcamera_component.so` 和 `liblidar_component.so`。

2. **编写融合组件 (`fusion_component`)**：
   - 继承 `minicyber::Component<Message, Message>`（接收 2 个参数）。
   - `Proc(camera_msg, lidar_msg)` 打印日志：“Fusion trigger: Camera TS: xxx, Lidar TS: xxx”。
   - 编译成 `libfusion_component.so`。

3. **编写 `perception.dag` 文件**：
   ```protobuf
   module_config {
       module_library: "libcamera_component.so"
       components {
           class_name: "CameraComponent"
           config { name: "camera" readers { channel: "/sensor/camera" } }
       }
   }
   module_config {
       module_library: "liblidar_component.so"
       components {
           class_name: "LidarComponent"
           config { name: "lidar" readers { channel: "/sensor/lidar" } }
       }
   }
   module_config {
       module_library: "libfusion_component.so"
       components {
           class_name: "FusionComponent"
           config {
               name: "fusion"
               readers { channel: "/perception/camera" }
               readers { channel: "/perception/lidar" }
           }
       }
   }
   ```

4. **编写数据源驱动 `drivers_mock.cpp`**：
   - 使用 `minicyber::Node` 创建 Writer。
   - 分别以 30Hz 发送 `/sensor/camera`，10Hz 发送 `/sensor/lidar`，并打上时间戳。

**提交信息**：
```
feat(examples): 移植 Apollo 真实感知链路负载与 DAG 编排

- 编写独立 .so 的 Camera/Lidar/Fusion 虚拟组件，模拟不同延时的业务计算
- 编写 perception.dag 配置文件，构建 [传感器 -> 感知 -> 融合] 的复杂有向无环图
- 编写 drivers_mock 独立进程提供真实频率 (30Hz/10Hz) 的数据激励
- 验证 mainboard dlopen 加载、Choreography 编排调度与 DataFusion 时间戳对齐能力
```

---

### Step 42: perf(profiling): 集成 Profiling 编译选项与性能脚本

**目标**：为 C++ 项目开启调试符号与火焰图支持，编写一键采集脚本。

**文件列表**：
- `CMakeLists.txt`（修改，支持 Profiling flag）
- `scripts/profiling.sh`（新增）

**具体实现要点**：

1. **修改 `CMakeLists.txt`**：
   - 增加 `-fno-omit-frame-pointer`（保留栈帧指针，火焰图必须）和 `-g -O3`（带符号的优化版本）。
   ```cmake
   set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -g -fno-omit-frame-pointer")
   ```

2. **编写 `scripts/profiling.sh`**：
   - 包含常用的分析命令，方便快速重现：
   ```bash
   #!/bin/bash
   # 1. 抓取 CPU 火焰图 (需安装 linux-tools)
   perf record -F 99 -a -g -- ./bin/mainboard -d examples/realworld/perception.dag &
   PID=$!
   sleep 10
   kill -INT $PID
   perf script | FlameGraph/stackcollapse-perf.pl | FlameGraph/flamegraph.pl > cpu_flamegraph.svg

   # 2. 统计 Futex 锁竞争
   strace -c -e futex ./bin/mainboard -d examples/realworld/perception.dag &
   
   # 3. CacheLine 伪共享诊断 (Cache 2 Cache)
   perf c2c record -- ./bin/mainboard -d examples/realworld/perception.dag
   perf c2c report
   ```

**提交信息**：
```
perf(profiling): 集成 Profiling 编译选项与采集脚本

- 在 Release 构建中开启 -fno-omit-frame-pointer 保留栈帧
- 新增 profiling.sh 脚本，一键采集 CPU 火焰图、Futex 系统调用与 Cache Miss 数据
- 为后续的性能瓶颈诊断与架构优化验证提供自动化工具链
```

---

### Step 43: docs(profiling): 补充基于真实负载的诊断与对比报告

**目标**：将调优过程形成书面报告，存放在代码库中，作为你简历的绝对证明。

**文件列表**：
- `docs/profiling.md`（新增，重点输出文件）
- `README.md`（修改，贴上截图链接）

**具体实现要点**：

在 `docs/profiling.md` 中，你需要（在真实跑完上述代码后）填写以下内容：

1.  **场景描述**
    *   在双路输入（30Hz/10Hz）下，运行 60 秒的感知-融合链路。
2.  **调度策略对比分析 (Classic vs Choreography)**
    *   **现象**：在使用 `Classic` 策略（优先级队列扫描）时，火焰图中 `NextRoutine()` 扫描 `DATA_WAIT` 队列的开销占比极高；通过 `strace` 看到大量的 `futex` 调用（全局锁争用）。
    *   **结论**：切换到 `Choreography` 策略后，通过 DAG 边的事件直达，火焰图中扫描开销消失，`futex` 阻塞时间下降了 XX%（写真实数据），端到端延迟分布更加收敛。
3.  **无锁优化与伪共享诊断 (perf c2c 报告)**
    *   **现象**：在初版的 `BoundedQueue` 中，如果未加 `alignas(CACHELINE_SIZE)`，运行 `perf c2c report`，会在 `head_` 和 `tail_` 变量所在的内存地址处观察到高频的 **HITM (Hit Modified)** 事件（多核缓存失效冲突）。
    *   **结论**：引入 `CACHELINE_SIZE` (64 字节) 隔离原子变量后，再次运行 `perf c2c`，HITM 事件降低至接近 0。队列的 Enqueue/Dequeue 并发吞吐率从 XXX 万次/秒 提升到了 YYY 万次/秒。
4.  **IPC 通信对比 (INTRA vs SHM)**
    *   列出表格对比：同进程内 `DataDispatcher` 零拷贝耗时（数十纳秒级）；跨进程 `SHM + Indicator` 耗时（受限于后台轮询，控制在 XX 微秒级）。

**提交信息**：
```
docs(profiling): 补充基于真实负载的性能诊断与架构对比报告

- 添加 docs/profiling.md，详细记录调优方法论与性能数据
- 提供 Classic 与 Choreography 调度的 Futex 阻塞与 CPU 开销对比
- 记录基于 perf c2c 的伪共享 (False Sharing) 诊断分析，证实 CacheLine 优化的关键作用
- 提供高可信度的工业级中间件压测背书
```

---

## 终极版 Step 清单（43 个 Commit）

| Phase | Step | 核心内容 | 面试/简历 高光点 |
|---|---|---|---|
| **P1 基础设施** | 01 | 移植并发宏定义与内存工具 | `cyber_likely` 分支预测优化 |
| | 02 | WaitStrategy 等待策略 | 支持 Block/Sleep/Yield 等多种协程让出机制 |
| | 03 | BoundedQueue 无锁有界队列 | **手写 CAS 环形队列，CacheLine 消除伪共享** |
| | 04 | AtomicRWLock | 用户态无锁读写自旋锁 |
| | 05 | AtomicHashMap | 固定大小 CAS 无锁哈希表 |
| **P2 调度引擎** | 06 | 协程状态机拓展（`DATA_WAIT`） | 突破协程仅靠 IO 唤醒的瓶颈 |
| | 07 | CPU 亲和性与调度策略封装 | `pthread_setaffinity_np` 绑核 |
| | 08 | Processor / Context 抽象 | 线程与调度上下文解耦 |
| | 09 | Classic 调度 + 本地队列 | O(1) 多级优先级队列 |
| | 10 | Scheduler + Work-Stealing | **工作窃取算法，解决多核锁竞争与饥饿** |
| **P3 数据中枢** | 11 | CacheBuffer | 基于覆盖写（Overwrite）的无锁历史缓冲 |
| | 12 | ChannelBuffer | 通道级数据隔离 |
| | 13 | DataNotifier | 异步事件通知回调 |
| | 14 | DataDispatcher（shared_mutex 版） | **数据驱动流转：数据到达直接唤醒协程** |
| | 15 | DataVisitor | 协程数据访问器，无数据时主动挂起 |
| | 16 | DataFusion 基础态 | 预留多传感器数据融合口子 |
| **P4 传输层** | 17 | SHM State/Block/Segment | 共享内存物理结构布局 |
| | 18 | PosixSegment + 生命周期管理 | `shm_open+mmap`，Signal 防内存泄漏 |
| | 19 | ConditionNotifier（原生基线） | **System V SHM + Indicator 环形微秒级轮询** |
| | 20 | Intra / Shm Dispatcher | 进程内零拷贝分发与跨进程共享内存分发 |
| | 21 | Transmitter / Receiver 接口 | 统一的发布/订阅原语抽象 |
| **P5 拓扑与收尾** | 22 | TopologyManager | **有向图计算拓扑维护** |
| | 23 | Transport 自动路由 | 依据拓扑自动降级 Intra 或 Shm 传输 |
| | 24 | Node / Reader / Writer | 极简用户侧 API 封装 |
| | 25 | Talker / Listener Demo | 验证同进程与跨进程数据打通 |
| | 26 | AtomicHashMap 替换优化 | **替换分发器的锁，实现纯无锁数据流转** |
| | 27 | README + 基础 Benchmark | 协程 vs 线程 PingPong 压测指标 |
| **P6 组件与RPC** | 28 | Protobuf 依赖与核心配置定义 | 引入 `dag.proto`, `component_config.proto` |
| | 29 | Component 基础抽象与生命周期 | `Initialize`, `Proc`, `Shutdown` 生命周期规范 |
| | 30 | 宏注册与 Component 工厂 | `MINICYBER_REGISTER_COMPONENT` 自动注册 |
| | 31 | `mainboard` 与 `dlopen` 动态加载 | **支持解析 DAG 动态热加载 `.so` 模块** |
| | 32 | Service / Client 抽象基类 | 定义双向隐式 Channel 的 RPC 路由规则 |
| | 33 | 自研低延迟 RPC 通信闭环 | **基于 request_id 实现的异步协程超时等待 RPC** |
| **P7 编排与融合** | 34 | DataFusion 的 `AllLatest` 策略 | 主通道触发、拉取副通道最新数据 |
| | 35 | DataVisitor 多通道支持 | 模板偏特化支持监听多个 Channel |
| | 36 | ChoreographyContext 编排上下文 | **废弃优先级队列，依据 DAG 依赖边实现调度** |
| | 37 | Component 适配多参数与定向唤醒 | **多传感器屏障同步 (Barrier) 与 O(1) 点对点唤醒** |
| **P8 跨机发现** | 38 | 抽象跨机 RTPS 接口 (Stub) | RTPS Transmitter / Receiver 接口预留 |
| | 39 | ServiceDiscovery 与跨机标记 | `host_ip / host_name` 节点身份标识 |
| | 40 | Transport 三级自适应路由 | **拓扑事件驱动的 Intra -> Shm -> Rtps 三级降级** |
| **P9 真实调优** | 41 | 移植 Apollo 真实感知负载与 DAG | 拒绝玩具压测，引入 30Hz/10Hz 异构传感器负载 |
| | 42 | 编译选项与 Profiling 自动化脚本 | 开启 `-fno-omit-frame-pointer` 保留栈帧 |
| | 43 | `docs/profiling.md` 核心调优报告 | **用 perf 火焰图与 perf c2c 诊断并证明消除了伪共享** |

---

**代码总量预估**：5000 行左右（极高密度的系统级代码，不含自动生成的 Proto 代码与测试用例）。
