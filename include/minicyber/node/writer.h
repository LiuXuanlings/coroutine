#ifndef MINICYBER_NODE_WRITER_H_
#define MINICYBER_NODE_WRITER_H_

#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

#include <google/protobuf/message.h>

#include "minicyber/proto/role_attributes.pb.h"
#include "minicyber/topology/topology_manager.h"
#include "minicyber/transport/transport.h"

namespace minicyber {
namespace node {

// Writer 对齐 CyberRT 的端点生命周期：先创建会订阅发现变化的 Hybrid
// transmitter，再 Join ChannelManager；关闭顺序反转为先停 Transport、再 Leave，
// 保证动态库卸载前不会再收到 opposite 的拓扑回调。
template <typename T>
class Writer {
  static_assert(std::is_base_of<google::protobuf::Message, T>::value,
                "Node Channel messages must derive from google::protobuf::Message");

 public:
  explicit Writer(proto::RoleAttributes role_attr)
      : role_attr_(std::move(role_attr)) {}
  ~Writer() { Shutdown(); }

  bool Init() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (init_) return true;
    transmitter_ = transport::Transport::CreateHybridTransmitter<T>(role_attr_);
    if (transmitter_ == nullptr ||
        !topology::TopologyManager::Instance()->Join(proto::ROLE_WRITER,
                                                      role_attr_)) {
      if (transmitter_) transmitter_->Disable();
      transmitter_.reset();
      return false;
    }
    init_ = true;
    return true;
  }

  bool Write(const std::shared_ptr<T>& msg) {
    std::shared_ptr<transport::HybridTransmitter<T>> transmitter;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      if (!init_ || msg == nullptr) return false;
      transmitter = transmitter_;
    }
    return transmitter != nullptr && transmitter->Transmit(msg);
  }

  bool Write(const T& msg) { return Write(std::make_shared<T>(msg)); }

  void Shutdown() {
    std::shared_ptr<transport::HybridTransmitter<T>> transmitter;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      if (!init_) return;
      init_ = false;
      transmitter = std::move(transmitter_);
    }
    if (transmitter) transmitter->Disable();
    topology::TopologyManager::Instance()->Leave(proto::ROLE_WRITER, role_attr_);
  }

  bool IsInit() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return init_;
  }
  bool HasReader() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return init_ && topology::TopologyManager::Instance()->HasReader(
                        role_attr_.channel_name());
  }
  const std::string& channel() const { return role_attr_.channel_name(); }
  const proto::RoleAttributes& role_attr() const { return role_attr_; }

 private:
  proto::RoleAttributes role_attr_;
  mutable std::mutex lifecycle_mutex_;
  std::shared_ptr<transport::HybridTransmitter<T>> transmitter_;
  bool init_ = false;
};

}  // namespace node
}  // namespace minicyber

#endif  // MINICYBER_NODE_WRITER_H_
