// MiniCyber mainboard 保留 cyber_ref mainboard 的“参数 -> 初始化 -> 加载 ->
// 等待 -> 清理”职责。DAG 与 Scheduler 配置独立，同一 DAG 只通过
// 替换 Scheduler 配置选择 Classic 或 Choreography。

#include <signal.h>
#include <unistd.h>

#include <pthread.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include <google/protobuf/text_format.h>

#include "minicyber/mainboard/module_controller.h"
#include "minicyber/proto/scheduler_conf.pb.h"
#include "minicyber/scheduler/scheduler_conf.h"
#include "minicyber/scheduler/scheduler_factory.h"
#include "minicyber/topology/topology_manager.h"
#include "minicyber/transport/dispatcher/shm_dispatcher.h"

namespace {

bool BlockShutdownSignals(sigset_t* shutdown_signals) {
  if (shutdown_signals == nullptr) return false;

  // 在创建 Scheduler 工作线程前阻塞退出信号，后续线程会
  // 继承该掩码。主线程随后用 sigwait 同步消费，避免“检查标志 -> pause”
  // 丢失信号，也避免 POSIX SHM 异常处理器覆盖进程 handler 后误把正常
  // SIGTERM 重新抛出。正常信号仍只由 mainboard 主线程触发有序析构。
  sigemptyset(shutdown_signals);
  sigaddset(shutdown_signals, SIGINT);
  sigaddset(shutdown_signals, SIGTERM);
  if (::pthread_sigmask(SIG_BLOCK, shutdown_signals, nullptr) != 0) {
    std::cerr << "[Mainboard] Cannot block shutdown signals." << std::endl;
    return false;
  }
  ::signal(SIGPIPE, SIG_IGN);
  return true;
}

void WaitForShutdown(const sigset_t& shutdown_signals) {
  int received_signal = 0;
  while (::sigwait(&shutdown_signals, &received_signal) == EINTR) {
  }
}

struct ProgramArgs {
  std::string dag_path;
  std::string scheduler_path;
  bool show_help = false;
  bool valid = true;
};

void PrintUsage(const char* prog_name) {
  std::cerr << "Usage: " << prog_name
            << " -d <dag_path> -s <scheduler_conf_path>\n"
            << "  -d, --dag <path>        唯一 DAG 文本配置\n"
            << "  -s, --scheduler <path>  独立 Scheduler 文本配置\n"
            << "  -h, --help              显示帮助\n";
}

ProgramArgs ParseArgs(int argc, char** argv) {
  ProgramArgs args;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      args.show_help = true;
      return args;
    }
    if (arg == "-d" || arg == "--dag" || arg == "-s" ||
        arg == "--scheduler") {
      if (i + 1 >= argc) {
        args.valid = false;
        return args;
      }
      std::string* destination =
          (arg == "-d" || arg == "--dag") ? &args.dag_path
                                             : &args.scheduler_path;
      if (!destination->empty()) {
        args.valid = false;
        return args;
      }
      *destination = argv[++i];
      continue;
    }
    args.valid = false;
    return args;
  }
  if (args.dag_path.empty() || args.scheduler_path.empty()) {
    args.valid = false;
  }
  return args;
}

bool ParseSchedulerFile(const std::string& path,
                        minicyber::scheduler::SchedulerConf* config) {
  std::ifstream input(path);
  if (!input.is_open()) {
    std::cerr << "[Mainboard] Cannot open Scheduler config: " << path
              << " - " << std::strerror(errno) << std::endl;
    return false;
  }
  std::string content((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
  minicyber::proto::SchedulerConf proto;
  if (!google::protobuf::TextFormat::ParseFromString(content, &proto) ||
      !minicyber::scheduler::SchedulerConf::FromProto(proto, config)) {
    std::cerr << "[Mainboard] Invalid Scheduler config: " << path
              << std::endl;
    return false;
  }
  return true;
}

void ShutdownRuntime(minicyber::mainboard::ModuleController* controller,
                     minicyber::scheduler::Scheduler* scheduler) {
  // 对齐 ComponentBase/TopologyManager/Scheduler 的生命周期门禁：插件中的
  // Component 先注销 Reader 与任务，再关闭 Transport/Discovery，最后才停调度。
  if (controller != nullptr) controller->Clear();
  minicyber::transport::ShmDispatcher::Instance()->Shutdown();
  minicyber::topology::TopologyManager::Instance()->Shutdown();
  if (scheduler != nullptr) scheduler->Shutdown();
}

}  // namespace

int main(int argc, char** argv) {
  const ProgramArgs args = ParseArgs(argc, argv);
  if (args.show_help) {
    PrintUsage(argv[0]);
    return 0;
  }
  if (!args.valid) {
    PrintUsage(argv[0]);
    return 1;
  }

  sigset_t shutdown_signals;
  if (!BlockShutdownSignals(&shutdown_signals)) return 1;

  minicyber::scheduler::SchedulerConf scheduler_conf;
  // Scheduler 在 DAG 之前完成解析和工厂创建，确保 Component::Initialize
  // 取得的 thread-local Scheduler 是命令行指定策略而非默认策略。
  if (!ParseSchedulerFile(args.scheduler_path, &scheduler_conf)) return 1;
  auto scheduler = minicyber::scheduler::SchedulerFactory::Create(scheduler_conf);
  if (!scheduler) {
    std::cerr << "[Mainboard] SchedulerFactory rejected configuration."
              << std::endl;
    return 1;
  }

  minicyber::mainboard::ModuleController controller({args.dag_path});
  if (!controller.LoadAll()) {
    ShutdownRuntime(&controller, scheduler.get());
    return 1;
  }
  WaitForShutdown(shutdown_signals);
  ShutdownRuntime(&controller, scheduler.get());
  return 0;
}
