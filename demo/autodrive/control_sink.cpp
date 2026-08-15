#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <vector>

#include "minicyber/node/node.h"
#include "minicyber/proto/autodrive.pb.h"
#include "minicyber/time/rate.h"
#include "minicyber/time/time.h"
#include "minicyber/topology/topology_manager.h"
#include "minicyber/transport/dispatcher/shm_dispatcher.h"
#include "minicyber/transport/transport.h"

namespace {

volatile sig_atomic_t g_shutdown_requested = 0;

void SignalHandler(int) { g_shutdown_requested = 1; }

struct ProgramArgs {
  uint64_t messages = 1000;
  uint64_t timeout_ms = 30000;
  bool metrics = false;
  bool cleanup_shm = false;
  std::string output_path;
  std::string ready_path;
  bool show_help = false;
  bool valid = true;
};

struct Metrics {
  uint64_t received = 0;
  uint64_t expected_sequence = 1;
  uint64_t missing = 0;
  uint64_t out_of_order = 0;
  uint64_t first_receive_ns = 0;
  uint64_t last_receive_ns = 0;
  std::vector<uint64_t> latency_ns;
};

void PrintUsage(const char* program) {
  std::cerr << "Usage: " << program
            << " [--messages <count>] [--timeout-ms <ms>] [--metrics]"
               " [--output <metrics_path>] [--ready-file <path>]"
               " [--cleanup-shm]\n";
}

bool ParseUnsigned(const std::string& value, uint64_t* result) {
  if (result == nullptr || value.empty()) return false;
  try {
    size_t parsed = 0;
    const uint64_t number = std::stoull(value, &parsed);
    if (parsed != value.size() || number == 0) return false;
    *result = number;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

ProgramArgs ParseArgs(int argc, char** argv) {
  ProgramArgs args;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "-h" || argument == "--help") {
      args.show_help = true;
      return args;
    }
    if (argument == "--metrics") {
      args.metrics = true;
      continue;
    }
    if (argument == "--cleanup-shm") {
      args.cleanup_shm = true;
      continue;
    }
    if ((argument != "--messages" && argument != "--timeout-ms" &&
         argument != "--output" && argument != "--ready-file") ||
        index + 1 >= argc) {
      args.valid = false;
      return args;
    }
    const std::string value = argv[++index];
    if ((argument == "--messages" && !ParseUnsigned(value, &args.messages)) ||
        (argument == "--timeout-ms" && !ParseUnsigned(value, &args.timeout_ms))) {
      args.valid = false;
      return args;
    }
    if (argument == "--output") args.output_path = value;
    if (argument == "--ready-file") args.ready_path = value;
  }
  if (!args.output_path.empty() && !args.metrics) args.valid = false;
  return args;
}

bool CleanupPipelineShm() {
  // 这是启动脚本在全部业务进程退出后调用的精确回收路径。只触及唯一
  // 自动驾驶主干的四个固定频道，不能用 /dev/shm/minicyber_* 通配符误删
  // 其他实例；正常生命周期仍由 PosixSegment 的引用计数负责。
  constexpr const char* kChannels[] = {
      "/autodrive/camera", "/autodrive/vehicle_state",
      "/autodrive/control_command", "/autodrive/control_audit"};
  bool success = true;
  for (const char* channel : kChannels) {
    const std::string name =
        "/minicyber_" + std::to_string(
                             minicyber::transport::Transport::ChannelNameToId(channel));
    if (::shm_unlink(name.c_str()) != 0 && errno != ENOENT) {
      std::cerr << "ControlSink could not remove " << name << ": "
                << std::strerror(errno) << ".\n";
      success = false;
    }
  }
  return success;
}

bool WriteReadyFile(const std::string& path) {
  if (path.empty()) return true;
  std::ofstream output(path);
  if (!output.is_open()) return false;
  output << "ready\n";
  return output.good();
}

bool WaitForWriter(const std::shared_ptr<minicyber::node::Reader<
                       minicyber::proto::ControlCommand>>& reader,
                   uint64_t timeout_ms) {
  const uint64_t deadline = minicyber::time::Time::MonoTime().ToNanosecond() +
                            timeout_ms * 1000000ULL;
  minicyber::time::Rate poll_rate(100.0);
  while (g_shutdown_requested == 0 &&
         minicyber::time::Time::MonoTime().ToNanosecond() < deadline) {
    // Sink 的 Writer Join 是整个链路的下游就绪事实。只有它已到达，脚本才
    // 放行 Source，避免输入已发送而 ControlCommand 的 SHM 对端尚未建立。
    if (reader->HasWriter()) return true;
    poll_rate.Sleep();
  }
  return false;
}

uint64_t Percentile(std::vector<uint64_t> values, uint64_t numerator,
                    uint64_t denominator) {
  if (values.empty() || denominator == 0) return 0;
  std::sort(values.begin(), values.end());
  const uint64_t index =
      (numerator * values.size() + denominator - 1) / denominator - 1;
  return values[std::min<uint64_t>(index, values.size() - 1)];
}

void PrintMetrics(const Metrics& metrics, std::ostream* output) {
  if (output == nullptr) return;
  const double elapsed_seconds =
      metrics.last_receive_ns > metrics.first_receive_ns
          ? static_cast<double>(metrics.last_receive_ns - metrics.first_receive_ns) /
                1000000000.0
          : 0.0;
  const double throughput = elapsed_seconds > 0.0
                                ? static_cast<double>(metrics.received) / elapsed_seconds
                                : 0.0;
  *output << "METRICS received=" << metrics.received
          << " missing=" << metrics.missing
          << " out_of_order=" << metrics.out_of_order
          << " throughput_mps=" << std::fixed << std::setprecision(3)
          << throughput << " latency_ns_p50="
          << Percentile(metrics.latency_ns, 50, 100)
          << " latency_ns_p95=" << Percentile(metrics.latency_ns, 95, 100)
          << " latency_ns_p99=" << Percentile(metrics.latency_ns, 99, 100)
          << "\n";
}

void ShutdownRuntime(minicyber::node::Node* node) {
  if (node != nullptr) node->Shutdown();
  minicyber::transport::ShmDispatcher::Instance()->Shutdown();
  minicyber::topology::TopologyManager::Instance()->Shutdown();
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
  if (args.cleanup_shm) {
    return CleanupPipelineShm() ? 0 : 7;
  }

  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  std::mutex mutex;
  std::condition_variable received_cv;
  Metrics metrics;
  minicyber::node::Node sink("autodrive_control_sink");
  auto reader = sink.CreateReader<minicyber::proto::ControlCommand>(
      "/autodrive/control_command",
      [&mutex, &received_cv, &metrics](
          const std::shared_ptr<minicyber::proto::ControlCommand>& command) {
        const uint64_t receive_time =
            minicyber::time::Time::MonoTime().ToNanosecond();
        std::lock_guard<std::mutex> lock(mutex);
        if (metrics.received == 0) metrics.first_receive_ns = receive_time;
        metrics.last_receive_ns = receive_time;
        ++metrics.received;
        if (command->source_sequence() > metrics.expected_sequence) {
          metrics.missing += command->source_sequence() - metrics.expected_sequence;
          metrics.expected_sequence = command->source_sequence() + 1;
        } else if (command->source_sequence() == metrics.expected_sequence) {
          ++metrics.expected_sequence;
        } else {
          ++metrics.out_of_order;
        }
        if (receive_time >= command->source_monotonic_ns()) {
          metrics.latency_ns.push_back(receive_time -
                                       command->source_monotonic_ns());
        }
        received_cv.notify_all();
      });
  if (reader == nullptr) {
    std::cerr << "ControlSink could not create ControlCommand reader.\n";
    ShutdownRuntime(&sink);
    return 2;
  }
  if (!WaitForWriter(reader, args.timeout_ms)) {
    std::cerr << "ControlSink timed out waiting for ControlCommand writer.\n";
    ShutdownRuntime(&sink);
    return 3;
  }
  // Reader 建立 SHM 段后恢复本进程的正常退出通知；异常 SHM 回收不由 Sink
  // 信号处理器承担，脚本会在子进程回收后执行精确频道清理。
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
  if (!WriteReadyFile(args.ready_path)) {
    std::cerr << "ControlSink cannot write ready file: " << args.ready_path
              << ".\n";
    ShutdownRuntime(&sink);
    return 4;
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(args.timeout_ms);
  std::unique_lock<std::mutex> lock(mutex);
  while (metrics.received < args.messages && g_shutdown_requested == 0) {
    // 信号处理器只能写 sig_atomic_t，不能通知 condition_variable；以短周期
    // 醒来检查退出标志，保证启动脚本的 trap 不会被完整业务超时拖住。
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) break;
    const auto wake_deadline = std::min(
        deadline, now + std::chrono::milliseconds(100));
    received_cv.wait_until(lock, wake_deadline);
  }
  Metrics result = metrics;
  // 收包提前结束时，尚未抵达的尾部序列同样属于缺号；否则只有中间跳号
  // 会进入指标，无法如实报告 Writer 过早关闭造成的尾部丢失。
  if (result.expected_sequence <= args.messages) {
    result.missing += args.messages - result.expected_sequence + 1;
  }
  const bool complete = result.received == args.messages;
  const bool orderly = result.missing == 0 && result.out_of_order == 0;
  lock.unlock();

  if (args.metrics) {
    if (args.output_path.empty()) {
      PrintMetrics(result, &std::cout);
    } else {
      std::ofstream output(args.output_path);
      if (!output.is_open()) {
        std::cerr << "ControlSink cannot write metrics: " << args.output_path
                  << ".\n";
        ShutdownRuntime(&sink);
        return 4;
      }
      PrintMetrics(result, &output);
    }
  }

  ShutdownRuntime(&sink);
  if (g_shutdown_requested != 0) return 5;
  if (!complete) {
    std::cerr << "ControlSink timed out after " << result.received << " of "
              << args.messages << " commands.\n";
    return 3;
  }
  if (!orderly) {
    std::cerr << "ControlSink observed missing or out-of-order commands.\n";
    return 6;
  }
  return 0;
}
