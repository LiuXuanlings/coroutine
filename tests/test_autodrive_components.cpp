#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

#include <unistd.h>

#include <google/protobuf/text_format.h>

#include "minicyber/proto/autodrive_runtime.h"
#include "minicyber/component/component_factory.h"
#include "minicyber/mainboard/module_controller.h"
#include "minicyber/node/node.h"
#include "minicyber/proto/autodrive.pb.h"
#include "minicyber/proto/dag_conf.pb.h"
#include "minicyber/scheduler/scheduler.h"
#include "minicyber/topology/topology_manager.h"

#ifndef MINICYBER_AUTODRIVE_COMPONENTS_PATH
#error "MINICYBER_AUTODRIVE_COMPONENTS_PATH must be provided by CMake"
#endif

#ifndef MINICYBER_SOURCE_DIR
#error "MINICYBER_SOURCE_DIR must be provided by CMake"
#endif

namespace {

using minicyber::component::ComponentFactory;
using minicyber::mainboard::ModuleController;
using minicyber::node::Node;
using minicyber::proto::CameraFrame;
using minicyber::proto::ControlCommand;
using minicyber::proto::DagConfig;
using minicyber::proto::FusedObstacle;
using minicyber::proto::PerceptionObstacle;
using minicyber::proto::Trajectory;
using minicyber::proto::VehicleState;

class ScopedWorkingDirectory {
 public:
  explicit ScopedWorkingDirectory(const std::string& target) {
    char buffer[4096] = {};
    if (::getcwd(buffer, sizeof(buffer)) != nullptr) previous_ = buffer;
    changed_ = !previous_.empty() && ::chdir(target.c_str()) == 0;
  }
  ~ScopedWorkingDirectory() {
    if (changed_) ::chdir(previous_.c_str());
  }
  bool changed() const { return changed_; }

 private:
  std::string previous_;
  bool changed_ = false;
};

bool ReadDag(DagConfig* dag) {
  std::ifstream input(std::string(MINICYBER_SOURCE_DIR) +
                      "/config/autodrive/autodrive.dag");
  if (!input.is_open()) return false;
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  return google::protobuf::TextFormat::ParseFromString(text, dag);
}

template <typename Message>
std::shared_ptr<minicyber::node::Reader<Message>> MakeObserver(
    Node* node, const std::string& channel, std::mutex* mutex,
    std::condition_variable* ready, std::shared_ptr<Message>* result) {
  return node->CreateReader<Message>(
      channel, [mutex, ready, result](const std::shared_ptr<Message>& message) {
        std::lock_guard<std::mutex> lock(*mutex);
        *result = message;
        ready->notify_all();
      });
}

TEST(AutodriveComponentsTest, DlopenBuildsOneDagAndRunsDeterministicChain) {
  ScopedWorkingDirectory cwd(MINICYBER_SOURCE_DIR);
  ASSERT_TRUE(cwd.changed());

  DagConfig dag;
  ASSERT_TRUE(ReadDag(&dag));
  ASSERT_EQ(dag.module_config_size(), 1);
  ASSERT_EQ(dag.module_config(0).components_size(), 5);
  EXPECT_EQ(dag.module_config(0).module_library(),
            "libminicyber_autodrive_components.so");
  dag.mutable_module_config(0)->set_module_library(
      MINICYBER_AUTODRIVE_COMPONENTS_PATH);

  std::mutex mutex;
  std::condition_variable ready;
  std::shared_ptr<PerceptionObstacle> perception;
  std::shared_ptr<FusedObstacle> fused;
  std::shared_ptr<Trajectory> trajectory;
  std::shared_ptr<ControlCommand> command;
  std::shared_ptr<ControlCommand> audit;
  Node observer("autodrive_component_observer");
  ASSERT_NE(MakeObserver<PerceptionObstacle>(
                &observer, "/autodrive/perception_obstacle", &mutex, &ready,
                &perception),
            nullptr);
  ASSERT_NE(MakeObserver<FusedObstacle>(&observer, "/autodrive/fused_obstacle",
                                        &mutex, &ready, &fused),
            nullptr);
  ASSERT_NE(MakeObserver<Trajectory>(&observer, "/autodrive/trajectory", &mutex,
                                     &ready, &trajectory),
            nullptr);
  ASSERT_NE(MakeObserver<ControlCommand>(&observer, "/autodrive/control_command",
                                         &mutex, &ready, &command),
            nullptr);
  ASSERT_NE(MakeObserver<ControlCommand>(&observer, "/autodrive/control_audit",
                                         &mutex, &ready, &audit),
            nullptr);

  minicyber::scheduler::SchedulerConf scheduler_conf;
  scheduler_conf.thread_num = 1;
  minicyber::scheduler::Scheduler scheduler(scheduler_conf);
  ModuleController controller({});
  ASSERT_TRUE(controller.LoadModule(dag));
  EXPECT_EQ(controller.ComponentCount(), 5u);
  EXPECT_EQ(controller.LibraryCount(), 1u);
  EXPECT_TRUE(ComponentFactory::Instance()->Has("PerceptionComponent"));
  EXPECT_TRUE(ComponentFactory::Instance()->Has("FusionComponent"));
  EXPECT_TRUE(ComponentFactory::Instance()->Has("PlanningComponent"));
  EXPECT_TRUE(ComponentFactory::Instance()->Has("ControlComponent"));
  EXPECT_TRUE(ComponentFactory::Instance()->Has("ControlAuditComponent"));

  Node source("autodrive_component_source");
  auto vehicle_writer = source.CreateWriter<VehicleState>("/autodrive/vehicle_state");
  auto camera_writer = source.CreateWriter<CameraFrame>("/autodrive/camera");
  ASSERT_NE(vehicle_writer, nullptr);
  ASSERT_NE(camera_writer, nullptr);
  ASSERT_TRUE(vehicle_writer->HasReader());
  ASSERT_TRUE(camera_writer->HasReader());

  VehicleState warmup;
  warmup.set_source_sequence(0);
  warmup.set_source_monotonic_ns(1);
  warmup.set_speed_mps(4.0);
  warmup.set_steering_angle_rad(0.1);
  ASSERT_TRUE(vehicle_writer->Write(warmup));

  CameraFrame camera;
  camera.set_source_sequence(17);
  camera.set_source_monotonic_ns(123456789);
  camera.set_width(1000);
  camera.set_height(500);
  camera.set_image_data(std::string(50, 'c'));
  ASSERT_TRUE(camera_writer->Write(camera));

  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(ready.wait_for(lock, std::chrono::seconds(2), [&] {
      return perception != nullptr && fused != nullptr && trajectory != nullptr &&
             command != nullptr && audit != nullptr;
    }));
  }

  ASSERT_NE(perception, nullptr);
  EXPECT_EQ(perception->source_sequence(), 17u);
  EXPECT_EQ(perception->source_monotonic_ns(), 123456789u);
  EXPECT_DOUBLE_EQ(perception->distance_m(), 10.5);
  EXPECT_DOUBLE_EQ(perception->lateral_offset_m(), 0.5);

  ASSERT_NE(fused, nullptr);
  EXPECT_EQ(fused->source_sequence(), 17u);
  EXPECT_NEAR(fused->distance_m(), 10.1, 1e-12);
  EXPECT_NEAR(fused->lateral_offset_m(), 0.6, 1e-12);
  EXPECT_DOUBLE_EQ(fused->relative_speed_mps(), -4.0);

  ASSERT_NE(trajectory, nullptr);
  EXPECT_EQ(trajectory->source_sequence(), 17u);
  EXPECT_NEAR(trajectory->target_speed_mps(), 6.1, 1e-12);
  EXPECT_NEAR(trajectory->target_curvature(), 0.6 / 10.1, 1e-12);

  ASSERT_NE(command, nullptr);
  ASSERT_NE(audit, nullptr);
  EXPECT_EQ(command->source_sequence(), 17u);
  EXPECT_EQ(command->source_monotonic_ns(), 123456789u);
  EXPECT_NEAR(command->throttle(), 6.1 / 20.0, 1e-12);
  EXPECT_DOUBLE_EQ(command->brake(), 0.0);
  EXPECT_NEAR(command->steering_target(), 0.6 / 10.1, 1e-12);
  EXPECT_EQ(audit->SerializeAsString(), command->SerializeAsString());

  source.Shutdown();
  observer.Shutdown();
  // 观察副本必须先释放；Clear 会卸载业务 DSO，不能让测试在卸载后才销毁
  // 由组件发布的消息控制块。
  perception.reset();
  fused.reset();
  trajectory.reset();
  command.reset();
  audit.reset();
  controller.Clear();
  EXPECT_FALSE(ComponentFactory::Instance()->Has("PerceptionComponent"));
  EXPECT_FALSE(ComponentFactory::Instance()->Has("ControlAuditComponent"));
  scheduler.Shutdown();
  minicyber::topology::TopologyManager::Instance()->Shutdown();
}

}  // namespace
