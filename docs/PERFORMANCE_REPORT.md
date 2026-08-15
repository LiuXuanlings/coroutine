# MiniCyber 自动驾驶主干性能报告

## 测量范围

本报告记录 MC-623 在唯一自动驾驶主干上的最终验收测量。Classic 与 Choreography 使用同一
`config/autodrive/autodrive.dag`、同一 Release 构建和
`--messages 1000 --frequency 100 --timeout-ms 30000 --metrics --no-evidence`；二者只替换
Scheduler 配置。sequence 0 双回执预热和发现阶段不进入延迟样本，功能 evidence 明确关闭。

原始结果为 [Classic CSV](perf/autodrive_classic.csv)、[Classic JSON](perf/autodrive_classic.json)、
[Choreography CSV](perf/autodrive_choreography.csv) 和
[Choreography JSON](perf/autodrive_choreography.json)。每种配置执行一次，未挑选最好值；本报告
不把这两次当前宿主运行解释为通用性能排序。

## 环境与输入

| 项目 | 实际值 |
|---|---|
| 基线提交 | `72853c9534ff4b2e1451c989c268af175175a1c5` |
| 工作区状态 | `clean` |
| 构建类型 | Release |
| CPU | AMD Ryzen 7 5800H with Radeon Graphics，4 个在线逻辑 CPU |
| 内核 | Linux 6.8.0-124-generic |
| 编译器 | GCC 11.4.0 |
| FastRTPS | 2.5.0+ds-3 |
| Protobuf | 3.12.4 |
| 进程可用 CPU | 在线 4 个；`Cpus_allowed_list=0-127` |
| 采集时系统负载 | 1.22 / 1.65 / 1.88 |
| DAG SHA-256 | `b573a8c2128a4b753b1dbc462bd906197d57ab62518648ef684a2775c152a257` |
| 消息数与频率 | 1000 条，100 Hz |

Classic Scheduler 配置 SHA-256 为
`d7d3c66a12e92debb65b7606951de1b58a5099c236fa2a66cbad52e4dadef630`；Choreography
配置 SHA-256 为 `14862b7b819bfea03132cd0dd50d64dfe88eebd630a27d2b0ac035123b8d3414`。

## 测量结果

| 配置 | p50 | p95 | p99 | 吞吐 | 丢包 | 重复 | 乱序 | 审计差异 | 时长 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Classic | 1,067,722 ns | 2,042,769 ns | 3,058,242 ns | 100.125 条/秒 | 0 | 0 | 0 | 0 | 9,987,546,331 ns |
| Choreography | 1,130,411 ns | 2,040,484 ns | 3,102,364 ns | 100.094 条/秒 | 0 | 0 | 0 | 0 | 9,990,598,187 ns |

两次运行都在脚本的兜底 `shm_unlink` 之前通过四个精确频道的自然回收检查。端到端延迟从
CameraFrame 的 `source_monotonic_ns` 到 ControlSink 接收 ControlCommand 的同一
`CLOCK_MONOTONIC` 域计算；吞吐为第一条至最后一条测量结果间的成功消息数除以原始单调时钟时长。

## 解释与限制

测量提交之后只允许把本报告、台账与最终状态 amend 回同一个 MC-623 Commit；这些证据更新不
改变已测量的代码、配置、脚本或 Release 二进制输入，因此 JSON 保留实际测量基线 SHA。

测量事实仅表明在上述固定输入、CPU、负载和一次运行下，两种配置均完整传递了 1000 条消息，
没有观测到丢包、重复、乱序或 ControlAudit/ControlSink 集合差异。Classic 的共享优先级队列、
Choreography 的定向区与公共池、INTRA/SHM 的对端扇出是已由功能验收验证的机制边界；本报告
不从这两行数据推断任一机制必然更快。

本项目不将结果与旧 INTRA/SHM ping-pong、PIPE、互斥队列、C++20 coroutine 实验或 Apollo
CyberRT 数值横向比较。采集频率本身约束了结果吞吐，虚拟机调度、系统负载和单次样本都会影响
分位数；需要新的环境或负载结论时，必须重新运行
`scripts/collect_autodrive_performance.sh` 并保留新的原始 CSV/JSON。
