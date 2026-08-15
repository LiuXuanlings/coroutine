#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include "demo/autodrive/metrics.h"
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
  bool check_shm_clean = false;
  std::string output_path;
  std::string ready_path;
  std::string evidence_path;
  bool show_help = false;
  bool valid = true;
};

using minicyber::autodrive::Metrics;

void PrintUsage(const char* program) {
  std::cerr << "Usage: " << program
            << " [--messages <count>] [--timeout-ms <ms>] [--metrics]"
               " [--output <metrics_path>] [--ready-file <path>]"
               " [--evidence-file <path>]"
               " [--cleanup-shm] [--check-shm-clean]\n";
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
    if (argument == "--check-shm-clean") {
      args.check_shm_clean = true;
      continue;
    }
    if ((argument != "--messages" && argument != "--timeout-ms" &&
         argument != "--output" && argument != "--ready-file" &&
         argument != "--evidence-file") ||
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
    if (argument == "--evidence-file") args.evidence_path = value;
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

bool CheckPipelineShmClean() {
  constexpr const char* kChannels[] = {
      "/autodrive/camera", "/autodrive/vehicle_state",
      "/autodrive/control_command", "/autodrive/control_audit"};
  bool clean = true;
  for (const char* channel : kChannels) {
    const std::string name =
        "/minicyber_" + std::to_string(
                             minicyber::transport::Transport::ChannelNameToId(channel));
    const int fd = ::shm_open(name.c_str(), O_RDWR, 0644);
    if (fd >= 0) {
      ::close(fd);
      std::cerr << "ControlSink found residual SHM segment " << name << ".\n";
      clean = false;
    } else if (errno != ENOENT) {
      std::cerr << "ControlSink could not inspect " << name << ": "
                << std::strerror(errno) << ".\n";
      clean = false;
    }
  }
  return clean;
}

bool WriteReadyFile(const std::string& path) {
  if (path.empty()) return true;
  std::ofstream output(path);
  if (!output.is_open()) return false;
  output << "ready\n";
  return output.good();
}

template <typename SequenceSet>
std::string JoinSequences(const SequenceSet& sequences) {
  std::vector<uint64_t> ordered_sequences(sequences.begin(), sequences.end());
  std::sort(ordered_sequences.begin(), ordered_sequences.end());
  std::ostringstream output;
  bool first = true;
  for (const uint64_t sequence : ordered_sequences) {
    if (!first) output << ',';
    output << sequence;
    first = false;
  }
  return output.str();
}

bool WriteEvidenceFile(const std::string& path, const Metrics& metrics,
                       uintptr_t control_pointer, uintptr_t audit_pointer) {
  if (path.empty()) return true;
  std::ofstream output(path);
  if (!output.is_open()) return false;
  // Sink 只观察跨进程 SHM 反序列化后的对象。进程号与地址同主板内的
  // Control/Audit 证据一起证明它不是同进程 INTRA shared_ptr 的别名。
  output << "sink_pid=" << ::getpid() << '\n';
  output << "control_sequences=" << JoinSequences(metrics.sequences) << '\n';
  output << "audit_sequences=" << JoinSequences(metrics.audit_sequences) << '\n';
  output << "control_first_pointer=" << control_pointer << '\n';
  output << "audit_first_pointer=" << audit_pointer << '\n';
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

void PrintMetrics(const Metrics& metrics, uint64_t expected_messages,
                  std::ostream* output) {
  if (output == nullptr) return;
  const uint64_t lost = expected_messages > metrics.sequences.size()
                            ? expected_messages - metrics.sequences.size()
                            : 0;
  uint64_t audit_difference = 0;
  for (const uint64_t sequence : metrics.sequences) {
    if (metrics.audit_sequences.count(sequence) == 0) ++audit_difference;
  }
  for (const uint64_t sequence : metrics.audit_sequences) {
    if (metrics.sequences.count(sequence) == 0) ++audit_difference;
  }
  const double elapsed_seconds =
      metrics.last_receive_ns > metrics.first_receive_ns
          ? static_cast<double>(metrics.last_receive_ns - metrics.first_receive_ns) /
                1000000000.0
          : 0.0;
  const double throughput = elapsed_seconds > 0.0
                                ? static_cast<double>(metrics.received) / elapsed_seconds
                                : 0.0;
  *output << "METRICS received=" << metrics.received
          << " audit_received=" << metrics.audit_received
          << " lost=" << lost
          << " duplicates=" << metrics.duplicates
          << " audit_duplicates=" << metrics.audit_duplicates
          << " out_of_order=" << metrics.out_of_order
          << " audit_difference=" << audit_difference
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
  if (args.check_shm_clean) {
    return CheckPipelineShmClean() ? 0 : 8;
  }

  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  std::mutex mutex;
  std::condition_variable received_cv;
  Metrics metrics;
  uintptr_t control_pointer = 0;
  uintptr_t audit_pointer = 0;
  minicyber::node::Node sink("autodrive_control_sink");
  auto reader = sink.CreateReader<minicyber::proto::ControlCommand>(
      "/autodrive/control_command",
      [&mutex, &received_cv, &metrics, &control_pointer,
       collect_metrics = args.metrics](
          const std::shared_ptr<minicyber::proto::ControlCommand>& command) {
        if (command->source_sequence() == 0) return;
        const uint64_t receive_time =
            minicyber::time::Time::MonoTime().ToNanosecond();
        std::lock_guard<std::mutex> lock(mutex);
        if (control_pointer == 0) {
          control_pointer = reinterpret_cast<uintptr_t>(command.get());
        }
        const uint64_t sequence = command->source_sequence();
        metrics.RecordMeasurement(sequence, command->source_monotonic_ns(),
                                  receive_time, collect_metrics);
        received_cv.notify_all();
      });
  auto audit_reader = sink.CreateReader<minicyber::proto::ControlCommand>(
      "/autodrive/control_audit",
      [&mutex, &received_cv, &metrics, &audit_pointer](
          const std::shared_ptr<minicyber::proto::ControlCommand>& command) {
        if (command->source_sequence() == 0) return;
        std::lock_guard<std::mutex> lock(mutex);
        if (audit_pointer == 0) {
          audit_pointer = reinterpret_cast<uintptr_t>(command.get());
        }
        metrics.RecordAudit(command->source_sequence());
        received_cv.notify_all();
      });
  if (reader == nullptr || audit_reader == nullptr) {
    std::cerr << "ControlSink could not create ControlCommand readers.\n";
    ShutdownRuntime(&sink);
    return 2;
  }
  if (!WaitForWriter(reader, args.timeout_ms) ||
      !WaitForWriter(audit_reader, args.timeout_ms)) {
    std::cerr << "ControlSink timed out waiting for Control or Audit writer.\n";
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
  while ((metrics.sequences.size() < args.messages ||
          metrics.audit_sequences.size() < args.messages) &&
         g_shutdown_requested == 0) {
    // 信号处理器只能写 sig_atomic_t，不能通知 condition_variable；以短周期
    // 醒来检查退出标志，保证启动脚本的 trap 不会被完整业务超时拖住。
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) break;
    const auto wake_deadline = std::min(
        deadline, now + std::chrono::milliseconds(100));
    received_cv.wait_until(lock, wake_deadline);
  }
  Metrics result = metrics;
  const uintptr_t result_control_pointer = control_pointer;
  const uintptr_t result_audit_pointer = audit_pointer;
  const uint64_t lost = args.messages > result.sequences.size()
                            ? args.messages - result.sequences.size()
                            : 0;
  const bool complete = result.sequences.size() == args.messages &&
                        result.audit_sequences.size() == args.messages;
  const bool orderly = lost == 0 && result.duplicates == 0 &&
                       result.audit_duplicates == 0 &&
                       result.out_of_order == 0 &&
                       result.sequences == result.audit_sequences;
  lock.unlock();

  if (args.metrics) {
    if (args.output_path.empty()) {
      PrintMetrics(result, args.messages, &std::cout);
    } else {
      std::ofstream output(args.output_path);
      if (!output.is_open()) {
        std::cerr << "ControlSink cannot write metrics: " << args.output_path
                  << ".\n";
        ShutdownRuntime(&sink);
        return 4;
      }
      PrintMetrics(result, args.messages, &output);
    }
  }

  if (!WriteEvidenceFile(args.evidence_path, result, result_control_pointer,
                         result_audit_pointer)) {
    std::cerr << "ControlSink cannot write evidence: " << args.evidence_path
              << ".\n";
    ShutdownRuntime(&sink);
    return 4;
  }

  ShutdownRuntime(&sink);
  if (g_shutdown_requested != 0) return 5;
  if (!complete) {
    std::cerr << "ControlSink timed out after " << result.sequences.size()
              << " control and " << result.audit_sequences.size() << " audit of "
              << args.messages << " commands.\n";
    return 3;
  }
  if (!orderly) {
    std::cerr << "ControlSink observed lost, duplicate, out-of-order, or audit-different commands.\n";
    return 6;
  }
  return 0;
}
