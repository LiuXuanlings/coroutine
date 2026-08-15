#include <algorithm>
#include <memory>

#include <sys/syscall.h>
#include <unistd.h>

#include "demo/autodrive/evidence.h"
#include "minicyber/proto/autodrive_runtime.h"
#include "minicyber/component/component.h"
#include "minicyber/component/component_factory.h"
#include "minicyber/proto/autodrive.pb.h"

namespace {

class PlanningComponent
    : public minicyber::component::Component<minicyber::proto::FusedObstacle> {
 protected:
  bool Init() override {
    minicyber::proto::ComponentConfig config;
    if (!GetProtoConfig(&config) || config.name() != "planning") return false;
    writer_ = node_->CreateWriter<minicyber::proto::Trajectory>("/autodrive/trajectory");
    return writer_ != nullptr;
  }

  bool Proc(const std::shared_ptr<minicyber::proto::FusedObstacle>& fused) override {
    auto* scheduler = minicyber::scheduler::Scheduler::GetThis();
    minicyber::autodrive::RuntimeEvidence::RecordComponent(
        "planning", fused->source_sequence(),
        static_cast<pid_t>(::syscall(SYS_gettid)),
        scheduler == nullptr ? -1 : scheduler->ProcessorTid(0),
        scheduler == nullptr ? -1 : scheduler->ProcessorTid(1));
    auto trajectory = minicyber::runtime::CreateAutodriveMessage<
        minicyber::proto::Trajectory>();
    trajectory->set_source_sequence(fused->source_sequence());
    trajectory->set_source_monotonic_ns(fused->source_monotonic_ns());
    trajectory->set_target_speed_mps(
        std::max(0.0, fused->distance_m() + fused->relative_speed_mps()));
    trajectory->set_target_curvature(
        fused->lateral_offset_m() / std::max(1.0, fused->distance_m()));
    trajectory->set_target_distance_m(fused->distance_m());
    return writer_->Write(trajectory);
  }

  void Clear() override { writer_.reset(); }

 private:
  std::shared_ptr<minicyber::node::Writer<minicyber::proto::Trajectory>> writer_;
};

MINICYBER_REGISTER_COMPONENT(PlanningComponent)

}  // namespace
