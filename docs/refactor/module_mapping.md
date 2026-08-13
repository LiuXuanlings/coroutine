# MC-002 模块映射与删除候选

记录日期：2026-08-13。唯一参考为当前 `cyber_ref/cyber` 源码；“已存在”只表示有对应文件，不表示行为已经对齐。后续任务必须以本表的参考路径为入口，并在完成时更新偏差状态。

## 首期模块映射

| 范围 | CyberRT 参考 | MiniCyber 当前路径 | 状态与主要偏差 | 后续任务 |
|---|---|---|---|---|
| CRoutine | `croutine/croutine.{h,cc}`、`croutine/detail/routine_context.{h,cc}`、`routine_factory.h` | `include/minicyber/croutine/croutine.h`、`src/croutine.cpp`、`include/minicyber/context.h`、`src/context.cpp`、`src/swap.S` | CRoutine 已有状态、锁和元数据雏形，但仍直接依赖旧根级 `context`，使用 `shared_ptr` thread-local 代替原生裸指针/主栈接口；无 `routine_context` 目录和 `routine_factory`。 | MC-101、MC-102 |
| Scheduler | `scheduler/{processor,processor_context,scheduler,scheduler_factory}.{h,cc}`、`scheduler/policy/{classic_context,scheduler_classic,choreography_context,scheduler_choreography}.{h,cc}`、`scheduler/common/{pin_thread,cv_wrapper,mutex_wrapper}.*` | `include/minicyber/scheduler/**`、`src/scheduler/**`；另有根级 `include/minicyber/scheduler.h`、`src/scheduler.cpp` | Processor、Context、顶层 Scheduler 与部分 pin-thread 已存在；无 SchedulerFactory、SchedulerClassic、SchedulerChoreography、CV/Mutex wrappers。根级 Fiber Scheduler 与新 `minicyber::scheduler::Scheduler` 并存。 | MC-003 至 MC-006、MC-103 至 MC-108 |
| Data | `data/{cache_buffer,channel_buffer,data_dispatcher,data_notifier,data_visitor,data_visitor_base}.h`、`data/fusion/{data_fusion,all_latest}.h` | `include/minicyber/data/**` | 文件结构基本对应；实现大多为头文件且未逐项验证容量、并发、观察和融合触发语义。 | MC-201 至 MC-204 |
| Transport（仅 INTRA、SHM） | `transport/transport.{h,cc}`、`common/{identity,endpoint}.*`、`dispatcher/{dispatcher,intra_dispatcher,shm_dispatcher}.*`、`{transmitter,receiver}/{transmitter,receiver,intra_*,shm_*}.h`、`message/*`、`shm/*` | `include/minicyber/transport/**`、`src/transport/{dispatcher,shm}/**`、`include/minicyber/topology/topology_manager.h` | 有 INTRA/SHM 收发器、部分 dispatcher、SHM 基础类型和静态 Transport 路由；缺通用 dispatcher、endpoint、完整 identity/message/qos 抽象及大部分 SHM factory/notifier/segment 变体。`src/transport/dispatcher/intra_dispatcher.cpp` 不存在。RTPS 参考代码不纳入首期。 | MC-205 至 MC-209、MC-301 |
| Node / Reader / Writer | `node/{node,node_channel_impl,node_service_impl,reader,reader_base,writer,writer_base}.{h,cc}` | `include/minicyber/node/{node,reader,writer}.h` | Node、Reader、Writer 已存在；无 NodeChannelImpl、NodeServiceImpl、ReaderBase、WriterBase，生命周期和持有关系尚未按原生实现核对。现有 service 是首期范围外的遗留扩展。 | MC-302、MC-303 |
| Component / DAG / mainboard | `component/{component_base,component,timer_component}.{h,cc}`、`mainboard/{module_argument,module_controller,mainboard}.{h,cc}`、`proto/{dag_conf,component_conf}.proto` | `include/minicyber/component/**`、`include/minicyber/mainboard/module_controller.h`、`src/mainboard/module_controller.cpp`、`bin/mainboard.cpp`、`include/minicyber/proto/{dag_conf,component_conf}.proto` | Component、TimerComponent、工厂、ModuleController、mainboard 与 DAG proto 均已有雏形；以自定义 ComponentFactory 替代 ClassLoader，移除了 ModuleArgument，`ComponentBase` 未接入原生 `scheduler::RemoveTask`。失败回滚、proto 兼容性与退出路径待验证。 | MC-304 至 MC-307 |

## 范围外参考

以下 `cyber_ref` 子树不作为首期对齐目标，不能因“缺失”而补入：`transport/rtps` 及 rtps dispatcher/receiver/transmitter、跨机 integration tests、`parameter`、`record`、监控工具、Python 相关能力。它们与路线图规定的本地 INTRA/SHM 核心范围不一致。

## 删除候选

删除顺序必须遵循任务卡；每项删除前重新执行所列引用检索，防止新路径仍有依赖。

| 任务 | 删除候选 | 当前唯一或主要引用 | 删除前核对 |
|---|---|---|---|
| MC-003 | `include/minicyber/fiber.h`、`src/fiber.cpp`、`tests/test_fiber.cpp` | 根级 `scheduler`、`iomanager`、旧 `context`；CRoutine 注释和桥接代码 | `rg -n 'minicyber/fiber.h|\\bFiber\\b' include src tests examples bin` |
| MC-004 | `include/minicyber/context.h`、`src/context.cpp`、`src/swap.S` 的旧调用边界 | `fiber.h`、`croutine/croutine.h`、`src/croutine.cpp` | `rg -n 'minicyber/context.h|\\b(context|MakeContext|SwapContext)\\b' include src tests` |
| MC-005 | `include/minicyber/iomanager.h`、`src/iomanager.cpp`、`tests/test_epoll.cpp`、`tests/example.cpp` | 仅旧 Scheduler/Fiber 栈及 epoll 示例/测试 | `rg -n 'minicyber/iomanager.h|\\bIOManager\\b' include src tests examples bin` |
| MC-006 | `include/minicyber/thread.h`、`src/thread.cpp`、`include/minicyber/scheduler.h`、`src/scheduler.cpp`、`tests/test_thread.cpp`、`tests/test_scheduler.cpp` | 旧 Fiber Scheduler 与 IOManager；新路径为 `include/minicyber/scheduler/**` | `rg -n 'minicyber/(thread|scheduler)\\.h|\\b(minicyber::Thread|minicyber::Scheduler)\\b' include src tests examples bin` |

`src/swap.S` 目前同时是旧 Context 的实现边界及 MC-002 基线构建失败位置；在 MC-004/MC-102 之间不得孤立删除。MC-003 至 MC-006 只移除旧根级架构，不能删除新 `include/minicyber/scheduler/` 与 `src/scheduler/` 子树。

## 人工链接检查

已执行以下检查：

```bash
find cyber_ref/cyber/{croutine,scheduler,data,transport,node,component,mainboard} \
  -type f \( -name '*.h' -o -name '*.cc' -o -name '*.proto' \) | sort
find include/minicyber src bin tests -type f \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.S' -o -name '*.proto' \) | sort
rg -n 'minicyber/(fiber|context|iomanager|thread|scheduler)\\.h|\\b(Fiber|Context|IOManager|Thread|Scheduler)\\b' \
  include src tests examples bin CMakeLists.txt
```

结果：首期六个范围均已关联到 MiniCyber 的明确路径；缺失项和旧架构依赖已列入上表，未发现需要跨范围新增的映射。构建引用由根级 `CMakeLists.txt` 的 `file(GLOB_RECURSE SRC_FILES "src/*.cpp" "src/*.S")` 自动收集，因此每个删除任务还必须检查 CTest 源文件列表与该 glob 的剩余内容。
