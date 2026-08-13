# MiniCyber

MiniCyber 是一个参考 Apollo CyberRT 本地核心链路的 C++17 实验性运行时。
当前范围包括 CRoutine、Classic/Choreography Scheduler、数据分发、Node
Reader/Writer、同进程 INTRA、显式跨进程 SHM、Component/DAG 和 `mainboard`。

不实现 RTPS/跨机通信、监控运维、参数服务、录包回放和 Python API。自动
`Transport` 工厂固定使用 INTRA；跨进程通信需调用方显式创建
`ShmTransmitter` 和 `ShmReceiver`。

## Build

依赖：CMake、支持 C++17 的编译器、Protobuf、pthread/dl/rt。测试依赖
GoogleTest；网络不可用时可用本地源码覆盖 FetchContent。

```bash
cmake --preset debug \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest
cmake --build build/debug -j2
ctest --test-dir build/debug --output-on-failure
```

可用预设：`debug`、`release`、`asan`、`tsan`。TSAN 配置可生成；在当前
宿主环境运行可能受地址映射限制，详见重构进度记录。

常用专项入口：

```bash
cmake --build build/debug --target check_stress
cmake --build build/debug --target check_cross_process
cmake --build build/debug --target check_scheduler_tsan
```

## Verified Capabilities

- `CRoutine` 生命周期、x86_64 上下文切换、Classic 和 Choreography 调度策略。
- Cache/Channel buffer、DataDispatcher/DataNotifier、AllLatest 数据融合。
- 同进程 Node `CreateReader`/`CreateWriter` 的 INTRA 通信与有界 Reader 历史。
- 显式 `ShmTransmitter`/`ShmReceiver` 的双进程共享内存通信和资源回收。
- Component、TimerComponent、DAG proto 校验、ModuleController 失败回滚与
  `mainboard` 启动入口。

实现范围和每项验证证据见
[`docs/refactor/02_架构取舍矩阵.md`](docs/refactor/02_架构取舍矩阵.md) 与
[`docs/refactor/00_进度记录.md`](docs/refactor/00_进度记录.md)。

## Same-Process Node Example

```cpp
#include <memory>
#include <string>

#include "minicyber/node/node.h"

int main() {
  minicyber::node::Node subscriber("subscriber");
  auto reader = subscriber.CreateReader<std::string>(
      "/chatter", [](const std::shared_ptr<std::string>& message) {
        // Consume *message.
      });

  minicyber::node::Node publisher("publisher");
  auto writer = publisher.CreateWriter<std::string>("/chatter");
  writer->Write(std::make_shared<std::string>("hello"));
}
```

This is an INTRA example. The included `talker` and `listener` programs are
simple local API examples; they are not an automatic cross-process transport
demo.

## Performance

The Release experiment measures SPSC one-way latency with one in-flight message
for INTRA, POSIX pipe, explicit SHM, and a mutex/condition-variable queue at
64 B, 1 KiB, and 64 KiB. Raw CSV/JSON, host metadata, commands, results, and
limitations are in
[`docs/refactor/perf/performance_report.md`](docs/refactor/perf/performance_report.md).
The data is one local-host measurement and is not a general mechanism ranking.

Reproduce a collection with:

```bash
cmake --preset release \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest
cmake --build build/release -j2 --target benchmark_pingpong
WARMUP=2000 SAMPLES=10000 \
  scripts/collect_benchmark.sh build/release/benchmark_pingpong \
  docs/refactor/perf/raw
```

`benchmark_cpp20_coroutine` is only a standalone C++20 compile/resume probe;
it is intentionally not included in the shared latency comparison.

## Layout

```text
include/minicyber/  public runtime, data, transport, node, component APIs
src/                implementations
tests/              GoogleTest/CTest coverage
examples/           API and benchmark executables
bin/mainboard.cpp   DAG/component launcher
docs/refactor/      mapping, progress records, and performance artifacts
```

## License

This project is for learning and interview demonstration. It references Apache
2.0 licensed Apollo CyberRT source structure; see the repository license files
for the applicable terms.
