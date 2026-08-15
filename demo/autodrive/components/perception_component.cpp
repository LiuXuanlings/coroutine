#include <memory>

#include "minicyber/proto/autodrive_runtime.h"
#include "minicyber/component/component.h"
#include "minicyber/component/component_factory.h"
#include "minicyber/proto/autodrive.pb.h"

namespace {

class PerceptionComponent
    : public minicyber::component::Component<minicyber::proto::CameraFrame> {
 protected:
  bool Init() override {
    minicyber::proto::ComponentConfig config;
    if (!GetProtoConfig(&config) || config.name() != "perception") return false;
    writer_ = node_->CreateWriter<minicyber::proto::PerceptionObstacle>(
        "/autodrive/perception_obstacle");
    return writer_ != nullptr;
  }

  bool Proc(const std::shared_ptr<minicyber::proto::CameraFrame>& frame) override {
    auto obstacle = minicyber::runtime::CreateAutodriveMessage<
        minicyber::proto::PerceptionObstacle>();
    // MC-614 的关联键只在 Source 写入；这里和后续组件只透传，保证 Sink 能计算端到端延迟。
    obstacle->set_source_sequence(frame->source_sequence());
    obstacle->set_source_monotonic_ns(frame->source_monotonic_ns());
    obstacle->set_obstacle_id(frame->source_sequence());
    obstacle->set_distance_m(frame->width() * 0.01 + frame->height() * 0.001);
    obstacle->set_lateral_offset_m(
        static_cast<double>(frame->image_data().size() % 100) / 100.0);
    obstacle->set_confidence(frame->image_data().empty() ? 0.0 : 1.0);
    return writer_->Write(obstacle);
  }

  void Clear() override { writer_.reset(); }

 private:
  std::shared_ptr<minicyber::node::Writer<minicyber::proto::PerceptionObstacle>> writer_;
};

MINICYBER_REGISTER_COMPONENT(PerceptionComponent)

}  // namespace
