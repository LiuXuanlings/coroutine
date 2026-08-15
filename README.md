# MiniCyber

MiniCyber 是一个面向 Linux x86_64、C++17 与高性能中间件学习的 CyberRT 精简实践。
项目保留栈式用户态协程、Classic/Choreography 双调度策略、Protobuf Channel、同进程
INTRA 零拷贝、跨进程 POSIX SHM、FastRTPS Channel 发现控制面，以及
`.dag + .so + dlopen` Component 编排；所有能力由同一条自动驾驶业务主干验证。

本项目不是完整 Apollo，也不支持跨主机 RTPS 数据、RPC、TimerComponent、参数服务、
录包回放、监控或 Python API。FastRTPS 只传递同机 Channel Join/Leave，业务数据面严格
限定为 INTRA 与 SHM。

## 主干链路

```text
SensorSource（独立进程，time::Rate）
  -> CameraFrame + VehicleState（SHM）
  -> PerceptionComponent
  -> PerceptionObstacle（INTRA）
  -> FusionComponent（双通道 AllLatest）
  -> FusedObstacle（INTRA）
  -> PlanningComponent
  -> Trajectory（INTRA）
  -> ControlComponent
  -> ControlCommand
       -> ControlAuditComponent（同进程 INTRA）
       -> ControlSink（独立进程 SHM）
```

mainboard 读取唯一的 `config/autodrive/autodrive.dag`，通过 ModuleController
动态加载 `libminicyber_autodrive_components.so`。Classic 与 Choreography 仅替换
Scheduler 配置，业务 DAG 和输入保持一致。Component 的 Proc 不在发布线程同步执行，
而是沿 `DataNotifier -> Scheduler::NotifyTask -> DATA_WAIT/READY -> CRoutine::Resume`
进入 Processor。

## 构建

系统前置依赖：CMake、支持 C++17 的 GCC、Protobuf、FastRTPS CMake Package、pthread、
`dl` 和 `rt`。FastRTPS 必须由系统提供，项目不会复制源码或自动下载替代包。GoogleTest
可通过本地源码目录提供，避免外部镜像不稳定：

```bash
cmake --preset debug \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest
cmake --build build/debug -j2
ctest --test-dir build/debug --output-on-failure
```

构建预设包括 `debug`、`release`、`asan` 和 `tsan`。TSAN 是否可运行取决于宿主地址映射；
若运行时报告 `unexpected memory mapping`，应按环境限制记录，不能伪报测试通过。

## 运行

默认业务验收为 1000 条、100 Hz，约运行 10 秒：

```bash
scripts/run_autodrive_pipeline.sh \
  --build-dir build/debug \
  --scheduler config/autodrive/classic_sched.conf \
  --messages 1000 --frequency 100 --metrics
```

Choreography 使用同一 DAG 和参数，只替换调度配置：

```bash
scripts/run_autodrive_pipeline.sh \
  --build-dir build/debug \
  --scheduler config/autodrive/choreo_sched.conf \
  --messages 1000 --frequency 100 --metrics --evidence
```

`--evidence` 用于功能验收，会记录线程、指针身份和逐组件序列集合；性能采集必须使用
`--no-evidence`，避免功能探针污染端到端延迟。

## 已验证边界

- `SwapContext` 满足 x86_64 System V ABI 栈入口约束，协程沿数据到达进行
  `DATA_WAIT -> READY` 唤醒。
- Classic 为调度组共享 20 级优先级队列；`Acquire()` 只防止同一协程并发执行。
- Choreography 同时保留定向 Processor 和 Classic 公共池。
- Channel 公开类型限定为 Protobuf Message；INTRA 传递同一个 `shared_ptr`，SHM 传递
  序列化字节并在接收端创建新对象。
- HybridTransport 按对端关系并存 INTRA 与 SHM，同一 Control Writer 同时服务本地 Audit
  和外部 Sink，且无重复投递。
- FastRTPS 控制面覆盖 Join/Leave、晚加入重放、Participant 异常离场与分步启动回滚。
- Component 通过真实 `.so + dlopen` 注册、实例化和卸载，关闭顺序为
  Shutdown、销毁组件、注销工厂条目、`dlclose`。

上述能力的精确源码链和证据边界见
[架构说明](docs/ARCHITECTURE.md)、[调用链手册](docs/CALL_CHAIN.md) 与
[验收口径](docs/refactor/03_验收与性能口径.md)。

## 性能

性能只测 Release 下的完整主干。SensorSource 写入源序列和单调时间，ControlSink 统计
p50/p95/p99、吞吐、丢包、重复、乱序及 Audit 集合差异。Classic 与 Choreography 使用
相同 DAG、1000 条/100 Hz 输入，各保留一次原始 CSV/JSON，不挑选最好值，也不与旧
ping-pong、PIPE、互斥队列或 Apollo 数值横向比较。

当前干净 Release 基线的一次结果为：Classic p50/p95/p99 分别为
1,067,722/2,042,769/3,058,242 ns，Choreography 为
1,130,411/2,040,484/3,102,364 ns；两者均为 1000 条完整接收、零丢包、零重复、零乱序、
零审计差异。吞吐受 100 Hz 输入节拍约束，不能据此推断某种调度策略普遍更快。

```bash
cmake --preset release \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest
cmake --build build/release -j2
scripts/collect_autodrive_performance.sh \
  --build-dir build/release --output-dir docs/perf \
  --messages 1000 --frequency 100 --timeout-ms 30000
```

实际环境、原始结果与限制见 [性能报告](docs/PERFORMANCE_REPORT.md)。

## 文档导航

- [路线图与 Agent 基线](MINICYBER_ROADMAP.md)
- [架构说明](docs/ARCHITECTURE.md)
- [自顶向下调用链](docs/CALL_CHAIN.md)
- [调试手册](docs/DEBUG_MANUAL.md)
- [面试问答](docs/INTERVIEW_QA.md)
- [性能报告](docs/PERFORMANCE_REPORT.md)
- [重构进度台账](docs/refactor/00_进度记录.md)

## 目录

```text
bin/                    mainboard 入口
config/autodrive/       唯一 DAG、组件配置和两种 Scheduler 配置
demo/autodrive/         SensorSource、ControlSink 与五个业务组件
include/minicyber/      中间件公开头文件与 Protobuf 协议
src/                    核心运行时实现
scripts/                唯一主干启动和性能采集脚本
tests/                  高风险单测与完整业务集成测试
docs/                   架构、调用链、调试、面试、性能和重构基线
```

项目用于学习与面试展示，设计职责参考 Apache 2.0 许可的 Apollo CyberRT 源码结构；
实际许可范围以仓库中的许可证文件为准。
