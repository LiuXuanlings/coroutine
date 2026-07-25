// =============================================================================
// MiniCyber mainboard — 框架启动入口
//
// 设计目标：对齐 Apollo CyberRT 的 mainboard 可执行文件，作为框架的标准化
//   启动入口。负责解析命令行参数、初始化调度环境、加载 DAG 配置、阻塞等待
//   退出信号并优雅清理。
//
// 启动流程：
//   1. ParseArgs(argc, argv) → 收集 -d <dag_path> 列表
//   2. 构造 Scheduler（默认 SchedulerConf，按 CPU 核数自动开线程）
//      → Phase 7 引入 Choreography 后，可从 DAG 配置中读取策略
//   3. ModuleController(dag_paths).LoadAll() → dlopen + 反射 + Initialize
//   4. WaitForShutdown() → 安装 SIGINT/SIGTERM 处理器，阻塞主线程
//   5. 收到信号 → Clear() → Scheduler::Shutdown() → 退出
//
// 与 CyberRT 的差异：
//   - 去掉 ModuleArgument 类（参数解析内联在 main 中，简化代码）
//   - 去掉 apollo::cyber::Init / WaitForShutdown 全局函数（用 std::atomic
//     + std::condition_variable 在本文件内实现，避免引入新的全局状态）
//   - 去掉 gflags 依赖（不解析 --flagfile 等参数）
//   - 信号处理器使用 sigaction 配合一个 self-pipe 或 atomic flag + CV notify
//
// 信号处理实现说明：
//   在异步信号上下文中直接操作 std::condition_variable 是未定义行为
//   （CV 内部使用 mutex，而 mutex 在信号处理函数中不是 async-signal-safe）。
//   因此这里采用「信号处理器只写 atomic flag + 主线程在 CV 上限时 wait」
//   的轮询式等待方案：wait_for(100ms) 周期性检查 flag，既保证可移植性
//   又避免在信号上下文中调用非 async-safe 函数。这是 glog WaitForShutdown
//   的同款做法。
// =============================================================================

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "minicyber/mainboard/module_controller.h"
#include "minicyber/scheduler/scheduler.h"

namespace {
// -----------------------------------------------------------------------------
// 全局退出信号状态
// -----------------------------------------------------------------------------
// std::atomic 在信号上下文中是 async-signal-safe 的（lock-free，无锁），
// 因此可以安全地在信号处理函数中写入。
// std::condition_variable 不是 async-signal-safe，不能在信号处理函数中
// 调用 notify_one/notify_all，所以主线程必须用 wait_for 周期性轮询 flag。
// -----------------------------------------------------------------------------
std::atomic<bool> g_shutdown_requested{false};
std::condition_variable g_shutdown_cv;
std::mutex g_shutdown_mutex;

void SignalHandler(int signum) {
  (void)signum;  // 不区分 SIGINT/SIGTERM，统一处理
  g_shutdown_requested.store(true, std::memory_order_relaxed);
}

void InstallSignalHandlers() {
  struct sigaction sa;
  sa.sa_handler = SignalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  // SA_RESTART 让被中断的系统调用自动重启（不影响 CV 轮询）
  sa.sa_flags |= SA_RESTART;
  ::sigaction(SIGINT, &sa, nullptr);
  ::sigaction(SIGTERM, &sa, nullptr);
  // 忽略 SIGPIPE：Writer 在跨机传输时可能遇到对端关闭，不应崩溃
  ::signal(SIGPIPE, SIG_IGN);
}

// -----------------------------------------------------------------------------
// WaitForShutdown — 阻塞主线程直到收到 SIGINT/SIGTERM
// -----------------------------------------------------------------------------
// wait_for(100ms) 周期性醒来检查 flag，避免在信号上下文中调用 CV notify。
// 100ms 的粒度足够灵敏（用户按下 Ctrl+C 后最多 100ms 退出），且 CPU 占用
// 极低（每秒 10 次唤醒）。
// -----------------------------------------------------------------------------
void WaitForShutdown() {
  std::unique_lock<std::mutex> lk(g_shutdown_mutex);
  while (!g_shutdown_requested.load(std::memory_order_relaxed)) {
    g_shutdown_cv.wait_for(lk, std::chrono::milliseconds(100));
  }
}

// -----------------------------------------------------------------------------
// 参数解析
// -----------------------------------------------------------------------------
struct ProgramArgs {
  std::vector<std::string> dag_paths;
  bool show_help = false;
};

void PrintUsage(const char* prog_name) {
  std::cerr
      << "Usage: " << prog_name << " -d <dag_path> [-d <dag_path> ...]\n"
      << "Options:\n"
      << "  -d <path>   Path to a DAG config file (text-format proto). May be\n"
      << "              specified multiple times to load multiple DAGs.\n"
      << "  -h          Show this help message and exit.\n"
      << "\n"
      << "Example:\n"
      << "  " << prog_name << " -d perception.dag -d planning.dag\n";
}

ProgramArgs ParseArgs(int argc, char** argv) {
  ProgramArgs args;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      args.show_help = true;
      return args;
    }
    if (arg == "-d" || arg == "--dag") {
      if (i + 1 >= argc) {
        std::cerr << "Error: " << arg << " requires a path argument.\n";
        args.show_help = true;  // 触发 usage 输出后退出
        return args;
      }
      args.dag_paths.emplace_back(argv[++i]);
    } else {
      std::cerr << "Warning: unknown argument '" << arg << "' ignored.\n";
    }
  }
  return args;
}

}  // namespace

// =============================================================================
// main
// =============================================================================

int main(int argc, char** argv) {
  ProgramArgs args = ParseArgs(argc, argv);

  if (args.show_help) {
    PrintUsage(argv[0]);
    return args.dag_paths.empty() ? 0 : 1;
  }

  if (args.dag_paths.empty()) {
    std::cerr << "Error: no DAG file specified. Use -d <path>.\n\n";
    PrintUsage(argv[0]);
    return 1;
  }

  // Step 1: 安装信号处理器（在启动任何线程之前，避免竞态）
  InstallSignalHandlers();

  // Step 2: 构造 Scheduler
  // 默认 SchedulerConf: thread_num=0 表示按 CPU 核数自动；policy=classic。
  // Phase 7 引入 Choreography 后，可从 DAG 配置中读取调度策略并切换。
  minicyber::scheduler::SchedulerConf sched_conf;
  std::unique_ptr<minicyber::scheduler::Scheduler> scheduler(
      new minicyber::scheduler::Scheduler(sched_conf));
  std::cout << "[Mainboard] Scheduler started with "
            << scheduler->ProcessorCount() << " processor(s)." << std::endl;

  // Step 3: 加载 DAG 配置
  minicyber::mainboard::ModuleController controller(args.dag_paths);
  if (!controller.LoadAll()) {
    std::cerr << "[Mainboard] ERROR: Failed to load DAG(s). Cleaning up."
              << std::endl;
    controller.Clear();
    return -1;
  }

  std::cout << "[Mainboard] All DAGs loaded successfully. "
            << "Press Ctrl+C to shut down." << std::endl;

  // Step 4: 阻塞等待退出信号
  WaitForShutdown();

  // Step 5: 优雅清理
  std::cout << "[Mainboard] Shutdown signal received. Cleaning up..."
            << std::endl;
  controller.Clear();
  scheduler->Shutdown();
  std::cout << "[Mainboard] Exit." << std::endl;
  return 0;
}