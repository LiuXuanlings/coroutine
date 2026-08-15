#include <memory>

#include "minicyber/proto/autodrive_runtime.h"
#include "minicyber/component/component.h"
#include "minicyber/component/component_factory.h"
#include "minicyber/proto/autodrive.pb.h"

namespace {

class ControlAuditComponent
    : public minicyber::component::Component<minicyber::proto::ControlCommand> {
 protected:
  bool Init() override {
    minicyber::proto::ComponentConfig config;
    if (!GetProtoConfig(&config) || config.name() != "control_audit") return false;
    writer_ = node_->CreateWriter<minicyber::proto::ControlCommand>(
        "/autodrive/control_audit");
    return writer_ != nullptr;
  }

  bool Proc(const std::shared_ptr<minicyber::proto::ControlCommand>& command) override {
    // Audit 不能更改控制语义；发布副本只为后续 Sink 与本地审计集合一致性提供可观察出口。
    auto audited = minicyber::runtime::CopyAutodriveMessage(*command);
    return writer_->Write(audited);
  }

  void Clear() override { writer_.reset(); }

 private:
  std::shared_ptr<minicyber::node::Writer<minicyber::proto::ControlCommand>> writer_;
};

MINICYBER_REGISTER_COMPONENT(ControlAuditComponent)

}  // namespace
