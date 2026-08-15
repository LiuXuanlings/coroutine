#include <algorithm>
#include <memory>

#include "minicyber/proto/autodrive_runtime.h"
#include "minicyber/component/component.h"
#include "minicyber/component/component_factory.h"
#include "minicyber/proto/autodrive.pb.h"

namespace {

class ControlComponent
    : public minicyber::component::Component<minicyber::proto::Trajectory> {
 protected:
  bool Init() override {
    minicyber::proto::ComponentConfig config;
    if (!GetProtoConfig(&config) || config.name() != "control") return false;
    writer_ = node_->CreateWriter<minicyber::proto::ControlCommand>(
        "/autodrive/control_command");
    return writer_ != nullptr;
  }

  bool Proc(const std::shared_ptr<minicyber::proto::Trajectory>& trajectory) override {
    auto command = minicyber::runtime::CreateAutodriveMessage<
        minicyber::proto::ControlCommand>();
    command->set_source_sequence(trajectory->source_sequence());
    command->set_source_monotonic_ns(trajectory->source_monotonic_ns());
    command->set_throttle(std::clamp(trajectory->target_speed_mps() / 20.0, 0.0, 1.0));
    command->set_brake(trajectory->target_speed_mps() <= 0.0 ? 1.0 : 0.0);
    command->set_steering_target(trajectory->target_curvature());
    return writer_->Write(command);
  }

  void Clear() override { writer_.reset(); }

 private:
  std::shared_ptr<minicyber::node::Writer<minicyber::proto::ControlCommand>> writer_;
};

MINICYBER_REGISTER_COMPONENT(ControlComponent)

}  // namespace
