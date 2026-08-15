#include <memory>

#include <sys/syscall.h>
#include <unistd.h>

#include "demo/autodrive/evidence.h"
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
    auto* scheduler = minicyber::scheduler::Scheduler::GetThis();
    minicyber::autodrive::RuntimeEvidence::RecordComponent(
        "control_audit", command->source_sequence(),
        static_cast<pid_t>(::syscall(SYS_gettid)),
        scheduler == nullptr ? -1 : scheduler->ProcessorTid(0),
        scheduler == nullptr ? -1 : scheduler->ProcessorTid(1));
    minicyber::autodrive::RuntimeEvidence::RecordAudit(
        command->source_sequence(), reinterpret_cast<uintptr_t>(command.get()));
    // Audit 不能更改控制语义；发布副本只为后续 Sink 与本地审计集合一致性提供可观察出口。
    auto audited = minicyber::runtime::CopyAutodriveMessage(*command);
    return writer_->Write(audited);
  }

  void Clear() override {
    // ModuleController 在 dlclose 前调用 Clear；此时 Source/Sink 已由脚本按
    // 完整集合排空，写出证据不会把可卸载 DSO 的对象泄露给关闭后的进程。
    minicyber::autodrive::RuntimeEvidence::Flush();
    writer_.reset();
  }

 private:
  std::shared_ptr<minicyber::node::Writer<minicyber::proto::ControlCommand>> writer_;
};

MINICYBER_REGISTER_COMPONENT(ControlAuditComponent)

}  // namespace
