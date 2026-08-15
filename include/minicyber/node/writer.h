#ifndef MINICYBER_NODE_WRITER_H_
#define MINICYBER_NODE_WRITER_H_

#include <memory>
#include <string>

#include "minicyber/topology/topology_manager.h"
#include "minicyber/transport/transmitter/transmitter.h"
#include "minicyber/transport/transport.h"

namespace minicyber {
namespace node {

// =============================================================================
// Writer：发布端用户接口（对齐 CyberRT Writer<M>）
//
// 职责：封装 Transmitter，提供极简的 Write(msg) API。
//   创建时自动向 TopologyManager 注册为 writer，使 Transport 路由可感知拓扑。
//
// 生命周期：
//   Init()    : 注册拓扑 + Transport::CreateTransmitter<T>
//   Write(msg): transmitter_->Transmit(msg)
//   Shutdown(): transmitter_->Disable()
//
// 与 CyberRT 的简化：
//   - 去掉 RoleAttributes / WriterBase / ChangeConnection / 动态拓扑监听
//   - 去掉 HasReader / GetReaders（依赖 ChannelManager，本移植无）
//   - 拓扑在 Init 时静态注册，不做动态变更通知
// =============================================================================

template <typename T>
class Writer {
 public:
  Writer(const std::string& node_name, const std::string& channel)
      : node_name_(node_name), channel_(channel) {}

  ~Writer() { Shutdown(); }

  // 注册拓扑 + 创建底层 Transmitter
  void Init() {
    if (init_) return;
    topology::TopologyManager::Instance()->AddChannelWriter(
        channel_, node_name_, ::getpid());
    transmitter_ = transport::Transport::CreateTransmitter<T>(channel_);
    init_ = true;
  }

  // 发布消息（shared_ptr 版本，零拷贝传递）
  bool Write(const std::shared_ptr<T>& msg) {
    if (!init_ || transmitter_ == nullptr || msg == nullptr) return false;
    return transmitter_->Transmit(msg);
  }

  // 发布消息（值版本，内部包装成 shared_ptr）
  bool Write(const T& msg) {
    return Write(std::make_shared<T>(msg));
  }

  void Shutdown() {
    if (!init_) return;
    init_ = false;
    if (transmitter_) {
      transmitter_->Disable();
      transmitter_.reset();
    }
  }

  bool IsInit() const { return init_; }
  const std::string& channel() const { return channel_; }

 private:
  std::string node_name_;
  std::string channel_;
  std::shared_ptr<transport::Transmitter<T>> transmitter_;
  bool init_ = false;
};

}  // namespace node
}  // namespace minicyber

#endif  // MINICYBER_NODE_WRITER_H_
