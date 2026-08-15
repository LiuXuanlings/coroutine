#include <memory>

#include "minicyber/proto/autodrive_runtime.h"
#include "minicyber/component/component.h"
#include "minicyber/component/component_factory.h"
#include "minicyber/proto/autodrive.pb.h"

namespace {

class FusionComponent
    : public minicyber::component::Component<minicyber::proto::PerceptionObstacle,
                                             minicyber::proto::VehicleState> {
 protected:
  bool Init() override {
    minicyber::proto::ComponentConfig config;
    if (!GetProtoConfig(&config) || config.name() != "fusion") return false;
    writer_ = node_->CreateWriter<minicyber::proto::FusedObstacle>(
        "/autodrive/fused_obstacle");
    return writer_ != nullptr;
  }

  bool Proc(const std::shared_ptr<minicyber::proto::PerceptionObstacle>& obstacle,
            const std::shared_ptr<minicyber::proto::VehicleState>& vehicle) override {
    auto fused = minicyber::runtime::CreateAutodriveMessage<
        minicyber::proto::FusedObstacle>();
    // AllLatest 的次通道只参与本次融合值；链路身份始终来自首通道的 CameraFrame。
    fused->set_source_sequence(obstacle->source_sequence());
    fused->set_source_monotonic_ns(obstacle->source_monotonic_ns());
    fused->set_obstacle_id(obstacle->obstacle_id());
    fused->set_distance_m(obstacle->distance_m() - vehicle->speed_mps() * 0.1);
    fused->set_lateral_offset_m(obstacle->lateral_offset_m() +
                                vehicle->steering_angle_rad());
    fused->set_relative_speed_mps(-vehicle->speed_mps());
    return writer_->Write(fused);
  }

  void Clear() override { writer_.reset(); }

 private:
  std::shared_ptr<minicyber::node::Writer<minicyber::proto::FusedObstacle>> writer_;
};

MINICYBER_REGISTER_COMPONENT(FusionComponent)

}  // namespace
