#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

#include "minicyber/node/node.h"
#include "minicyber/proto/autodrive.pb.h"
#include "minicyber/time/rate.h"
#include "minicyber/time/time.h"
#include "minicyber/topology/topology_manager.h"

namespace {

volatile sig_atomic_t g_shutdown_requested = 0;

void SignalHandler(int) { g_shutdown_requested = 1; }

struct ProgramArgs {
  uint64_t messages = 1000;
  double frequency = 100.0;
  uint64_t ready_timeout_ms = 30000;
  bool await_shutdown = false;
  bool show_help = false;
  bool valid = true;
};

void PrintUsage(const char* program) {
  std::cerr << "Usage: " << program
            << " [--messages <count>] [--frequency <hz>]"
               " [--ready-timeout-ms <ms>] [--await-shutdown]\n";
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

bool ParseFrequency(const std::string& value, double* result) {
  if (result == nullptr || value.empty()) return false;
  try {
    size_t parsed = 0;
    const double frequency = std::stod(value, &parsed);
    if (parsed != value.size() || frequency <= 0.0) return false;
    *result = frequency;
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
    if (argument == "--await-shutdown") {
      args.await_shutdown = true;
      continue;
    }
    if ((argument != "--messages" && argument != "--frequency" &&
         argument != "--ready-timeout-ms") ||
        index + 1 >= argc) {
      args.valid = false;
      return args;
    }
    const std::string value = argv[++index];
    if ((argument == "--messages" && !ParseUnsigned(value, &args.messages)) ||
        (argument == "--frequency" && !ParseFrequency(value, &args.frequency)) ||
        (argument == "--ready-timeout-ms" &&
         !ParseUnsigned(value, &args.ready_timeout_ms))) {
      args.valid = false;
      return args;
    }
  }
  return args;
}

bool WaitForReaders(const std::shared_ptr<minicyber::node::Writer<
                        minicyber::proto::CameraFrame>>& camera_writer,
                    const std::shared_ptr<minicyber::node::Writer<
                        minicyber::proto::VehicleState>>& vehicle_writer,
                    uint64_t timeout_ms) {
  const uint64_t deadline = minicyber::time::Time::MonoTime().ToNanosecond() +
                            timeout_ms * 1000000ULL;
  minicyber::time::Rate poll_rate(100.0);
  while (g_shutdown_requested == 0 &&
         minicyber::time::Time::MonoTime().ToNanosecond() < deadline) {
    // HasReader 以 ChannelManager 的远端 Join 为依据；不能以脚本 sleep
    // 代替，否则首个 CameraFrame 可能早于 Fusion 的 VehicleState 订阅。
    if (camera_writer->HasReader() && vehicle_writer->HasReader()) return true;
    poll_rate.Sleep();
  }
  return false;
}

void ShutdownRuntime(minicyber::node::Node* node) {
  if (node != nullptr) node->Shutdown();
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

  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  minicyber::node::Node source("autodrive_sensor_source");
  auto camera_writer = source.CreateWriter<minicyber::proto::CameraFrame>(
      "/autodrive/camera");
  auto vehicle_writer = source.CreateWriter<minicyber::proto::VehicleState>(
      "/autodrive/vehicle_state");
  if (camera_writer == nullptr || vehicle_writer == nullptr ||
      !WaitForReaders(camera_writer, vehicle_writer, args.ready_timeout_ms)) {
    std::cerr << "SensorSource timed out waiting for CameraFrame and "
                 "VehicleState readers.\n";
    ShutdownRuntime(&source);
    return 2;
  }
  // 已连接的 SHM 端点可能安装异常清理处理器；Source 的 SIGTERM 是脚本确认
  // 排空后的正常关闭信号，必须继续只写退出标志并走 Node 的有序析构。
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  minicyber::time::Rate rate(args.frequency);
  minicyber::proto::VehicleState warmup;
  warmup.set_source_sequence(0);
  warmup.set_source_monotonic_ns(
      minicyber::time::Time::MonoTime().ToNanosecond());
  warmup.set_speed_mps(4.0);
  warmup.set_steering_angle_rad(0.1);
  if (!vehicle_writer->Write(warmup)) {
    std::cerr << "SensorSource could not publish VehicleState warmup.\n";
    ShutdownRuntime(&source);
    return 3;
  }

  // Fusion 的次通道必须先有最新值。预热不属于测量序列，随后完整等待一个
  // Rate 周期再发首个 CameraFrame，与 CyberRT 数据驱动 Component 的 AllLatest
  // 主通道唤醒语义对齐。
  rate.Sleep();
  uint64_t published_messages = 0;
  for (uint64_t sequence = 1;
       sequence <= args.messages && g_shutdown_requested == 0; ++sequence) {
    const uint64_t source_time =
        minicyber::time::Time::MonoTime().ToNanosecond();
    minicyber::proto::VehicleState vehicle;
    vehicle.set_source_sequence(sequence);
    vehicle.set_source_monotonic_ns(source_time);
    vehicle.set_speed_mps(4.0 + static_cast<double>(sequence % 5));
    vehicle.set_steering_angle_rad(0.02 * static_cast<double>(sequence % 3));

    minicyber::proto::CameraFrame camera;
    camera.set_source_sequence(sequence);
    camera.set_source_monotonic_ns(source_time);
    camera.set_width(640);
    camera.set_height(480);
    camera.set_image_data(std::string(64, 'c'));

    // 每个测量序列严格先发布次通道 VehicleState，再发布主通道 CameraFrame；
    // 后者触发 Fusion 的 AllLatest 取值，不能交换该顺序或重写关联字段。
    if (!vehicle_writer->Write(vehicle) || !camera_writer->Write(camera)) {
      std::cerr << "SensorSource publish failed at sequence " << sequence
                << ".\n";
      ShutdownRuntime(&source);
      return 3;
    }
    published_messages = sequence;
    rate.Sleep();
  }

  if (g_shutdown_requested != 0 && published_messages != args.messages) {
    std::cerr << "SensorSource interrupted before all measurement messages "
                 "were published.\n";
    ShutdownRuntime(&source);
    return 4;
  }

  if (args.await_shutdown) {
    // SHM Writer 不能在最后一次 Write 后立即销毁：这会在独立 mainboard
    // 尚未消费尾部块时 Disable 发送端。脚本以 Sink 的完整序列确认为排空
    // 条件，然后用 SIGTERM 触发此处的有序关闭，保持 CyberRT 端点生命周期。
    std::cout << "SensorSource published all measurement messages; awaiting "
                 "pipeline shutdown.\n";
    minicyber::time::Rate shutdown_rate(100.0);
    while (g_shutdown_requested == 0) shutdown_rate.Sleep();
  }

  ShutdownRuntime(&source);
  return 0;
}
