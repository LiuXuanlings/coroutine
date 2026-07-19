# MiniCyber

一个参考百度 Apollo CyberRT 架构设计的轻量级数据驱动协程框架。

剥离了 FastRTPS 和 Protobuf 依赖，提取了 CyberRT 最核心的**数据驱动协程调度器**与**零拷贝传输层**。包含无锁并发组件、基于 Classic 策略的工作窃取调度、数据分发中枢，以及跨进程共享内存通信底座。

> 面试口径："这是一个参考 Apollo CyberRT 架构设计的自动驾驶/机器人高性能中间件。剥离了 FastRTPS 和 Protobuf 依赖，提取了最核心的数据驱动协程调度器与零拷贝传输层。包含无锁组件、基于 Classic 策略的工作窃取调度、数据分发中枢，以及跨进程共享内存通信底座。"

---

## 架构

```
  +-----------------------------------------------------------------------+
  |  Node  /  Reader  /  Writer  (用户 API 层)                            |
  |  CreateReader("/chatter", callback)  /  CreateWriter("/chatter")      |
  +-----------------------------------------------------------------------+
  |  Transport (自动路由层)                                                |
  |  IsSameProc? -> YES: IntraTransmitter/Receiver (零拷贝指针投递)       |
  |               -> NO:  ShmTransmitter/Receiver (shm_open + mmap)       |
  +-----------------------------------------------------------------------+
  |  DataDispatcher / DataNotifier / DataFusion (数据驱动中枢)            |
  |  写入 -> ChannelBuffer -> Notify -> 唤醒 DATA_WAIT 协程              |
  |  支持多通道屏障等待 (WaitForAllLatest)                                 |
  +-----------------------------------------------------------------------+
  |  Scheduler / Processor / ClassicContext (协程调度引擎)                |
  |  Work-Stealing / CPU 亲和性绑核 / 多级优先级队列                     |
  +-----------------------------------------------------------------------+
  |  BoundedQueue / AtomicHashMap / AtomicRWLock (无锁基础组件)           |
  |  CacheLine 对齐 / CAS 操作 / 无锁环形队列                             |
  +-----------------------------------------------------------------------+
```

## 快速开始

### 编译

```bash
cd build
cmake ..
make -j$(nproc)
```

使用 AddressSanitizer 检测内存错误：

```bash
cmake .. -DUSE_SANITIZER=ASAN
make -j$(nproc)
```

### 运行测试

```bash
cd build
ctest --output-on-failure
```

### 运行示例

终端 1 —— 发布端：

```bash
./build/talker
```

终端 2 —— 订阅端：

```bash
./build/listener
```

运行性能基准测试：

```bash
./build/benchmark_pingpong
```

---

## 示例代码

### Talker（发布端）

```cpp
#include "minicyber/node/node.h"
#include "minicyber/node/writer.h"

using minicyber::node::Node;

int main() {
  Node node("talker");
  auto writer = node.CreateWriter<std::string>("/chatter");

  for (int i = 0; i < 10; ++i) {
    auto msg = std::make_shared<std::string>("hello " + std::to_string(i));
    writer->Write(msg);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  return 0;
}
```

### Listener（订阅端）

```cpp
#include "minicyber/node/node.h"
#include "minicyber/node/reader.h"

using minicyber::node::Node;

int main() {
  Node node("listener");
  auto reader = node.CreateReader<std::string>(
      "/chatter", [](const std::shared_ptr<std::string>& msg) {
        std::cout << "received: " << *msg << std::endl;
      });

  std::this_thread::sleep_for(std::chrono::seconds(15));
  return 0;
}
```

---

## 性能数据

同进程 INTRA（DataDispatcher + DataNotifier 零拷贝） vs POSIX PIPE 对比：

| 指标 | INTRA | PIPE | 倍率 |
|------|-------|------|------|
| 平均延迟 | 0.27 us | 26.82 us | ~100x |
| 最小延迟 | 0.19 us | 2.10 us | ~11x |
| 最大延迟 | 103 us | 855 us | ~8x |
| Msg/s | 3,671,205 | 37,290 | ~98x |

> 测试环境：Linux 6.8, x86-64, ASan enabled, 64 bytes payload, 100k 次迭代。
> INTRA 延迟优势来自指针级零拷贝 + 同步回调（无线程切换）。

---

## 项目结构

```
minicyber/
├── CMakeLists.txt
├── include/minicyber/
│   ├── base/               # BoundedQueue, AtomicHashMap, WaitStrategy, AtomicRWLock
│   ├── croutine/           # CRoutine, RoutineState
│   ├── scheduler/          # Scheduler, Processor, ClassicContext, PinThread
│   ├── data/               # CacheBuffer, ChannelBuffer, DataDispatcher, DataNotifier, DataVisitor, DataFusion
│   ├── transport/          # Transport, Segment, ConditionNotifier, ShmDispatcher, IntraDispatcher, Transmitter, Receiver
│   ├── topology/           # TopologyManager
│   ├── node/               # Node, Reader, Writer
│   └── time/               # Time, Duration
├── src/
│   ├── croutine/
│   ├── scheduler/
│   ├── transport/
│   ├── topology/
│   └── node/
├── tests/                  # 单元测试 (GoogleTest)
└── examples/               # Talker, Listener, Benchmark
```

---

## CyberRT 源码索引

| MiniCyber Header | 对应 CyberRT 源文件 |
|---|---|
| `base/macros.h` | `cyber/base/macros.h` |
| `base/wait_strategy.h` | `cyber/base/wait_strategy.h` |
| `base/bounded_queue.h` | `cyber/base/bounded_queue.h` |
| `base/atomic_rw_lock.h` | `cyber/base/atomic_rw_lock.h` |
| `base/atomic_hash_map.h` | `cyber/base/atomic_hash_map.h` |
| `common/types.h` | `cyber/common/types.h` |
| `croutine/croutine.h` | `cyber/croutine/croutine.h` |
| `scheduler/common/pin_thread.h` | `cyber/scheduler/common/pin_thread.h` |
| `scheduler/processor.h` | `cyber/scheduler/processor.h` |
| `scheduler/scheduler.h` | `cyber/scheduler/scheduler.h` |
| `scheduler/policy/classic_context.h` | `cyber/scheduler/policy/classic_context.h` |
| `data/cache_buffer.h` | `cyber/data/cache_buffer.h` |
| `data/channel_buffer.h` | `cyber/data/channel_buffer.h` |
| `data/data_notifier.h` | `cyber/data/data_notifier.h` |
| `data/data_dispatcher.h` | `cyber/data/data_dispatcher.h` |
| `data/data_visitor.h` | `cyber/data/data_visitor.h` |
| `data/data_fusion.h` | `cyber/data/fusion/data_fusion.h` |
| `transport/shm/state.h` | `cyber/transport/shm/state.h` |
| `transport/shm/block.h` | `cyber/transport/shm/block.h` |
| `transport/shm/segment.h` | `cyber/transport/shm/segment.h` |
| `transport/shm/posix_segment.h` | `cyber/transport/shm/posix_segment.h` |
| `transport/shm/condition_notifier.h` | `cyber/transport/shm/condition_notifier.h` |
| `transport/transmitter/transmitter.h` | `cyber/transport/transmitter/transmitter.h` |
| `transport/receiver/receiver.h` | `cyber/transport/receiver/receiver.h` |
| `node/node.h` | `cyber/node/node.h` |
| `node/reader.h` | `cyber/node/reader.h` |
| `node/writer.h` | `cyber/node/writer.h` |

---

## 许可

本项目为学习与面试展示目的，参考了 Apache 2.0 许可的 Apollo CyberRT 源码。
