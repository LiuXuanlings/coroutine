#ifndef MINICYBER_TRANSPORT_TRANSMITTER_HYBRID_TRANSMITTER_H_
#define MINICYBER_TRANSPORT_TRANSMITTER_HYBRID_TRANSMITTER_H_

#include <mutex>
#include <type_traits>
#include <unordered_map>

#include <google/protobuf/message.h>

#include "minicyber/common/types.h"
#include "minicyber/proto/role_attributes.pb.h"
#include "minicyber/proto/topology_change.pb.h"
#include "minicyber/topology/topology_manager.h"
#include "minicyber/transport/transmitter/intra_transmitter.h"
#include "minicyber/transport/transmitter/shm_transmitter.h"

namespace minicyber {
namespace transport {

// HybridTransmitter 对齐 CyberRT 的 per-opposite 连接模型：同进程角色只
// 共享 IntraTransmitter，跨进程角色只共享 ShmTransmitter；两种连接可以
// 同时存在。TopologyManager 是连接状态输入，关闭时先摘除监听器再停后端，
// 保证动态拓扑回调不会访问已销毁的 transmitter。
template <typename M>
class HybridTransmitter : public Transmitter<M> {
  static_assert(std::is_base_of<google::protobuf::Message, M>::value,
                "HybridTransport messages must derive from google::protobuf::Message");

 public:
  using MessagePtr = typename Transmitter<M>::MessagePtr;
  using RoleAttributes = proto::RoleAttributes;

  explicit HybridTransmitter(const RoleAttributes& attr)
      : Transmitter<M>(attr.channel_id()), attr_(attr) {
    intra_ = std::make_shared<IntraTransmitter<M>>(this->channel_id_);
    shm_ = std::make_shared<ShmTransmitter<M>>(this->channel_id_);
    listener_ = topology::TopologyManager::Instance()->AddChangeListener(
        [this](const proto::ChangeMsg& msg) { OnTopologyChange(msg); });
    ReconcileInitial();
  }

  ~HybridTransmitter() override {
    topology::TopologyManager::Instance()->RemoveChangeListener(listener_);
    Disable();
  }

  void Enable() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (this->enabled_) return;
    this->enabled_ = true;
    ReconcileBackendsLocked();
  }

  void Disable() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!this->enabled_) return;
    this->enabled_ = false;
    intra_->Disable();
    shm_->Disable();
  }

  void Enable(const RoleAttributes& opposite_attr) {
    ApplyOpposite(opposite_attr, true);
  }

  void Disable(const RoleAttributes& opposite_attr) {
    ApplyOpposite(opposite_attr, false);
  }

  bool Transmit(const MessagePtr& msg) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!this->enabled_ || msg == nullptr) return false;
    bool delivered = false;
    if (intra_enabled_) delivered = intra_->Transmit(msg) || delivered;
    if (shm_enabled_) delivered = shm_->Transmit(msg) || delivered;
    // CyberRT 的 HybridTransmitter 对“无当前对端”也保持发布接口成功；
    // 路由是否有读者由 HasReader 在 Node 层观察。
    return delivered || opposite_.empty();
  }

  bool HasReader() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !opposite_.empty();
  }

 private:
  static uint64_t RoleId(const RoleAttributes& attr) {
    if (attr.has_id()) return attr.id();
    return std::hash<std::string>{}(attr.node_name() + attr.channel_name()) ^
           static_cast<uint64_t>(attr.process_id());
  }

  Relation RelationFor(const RoleAttributes& opposite) const {
    if (opposite.channel_name() != attr_.channel_name()) return NO_RELATION;
    if (opposite.has_host_name() && attr_.has_host_name() &&
        opposite.host_name() != attr_.host_name()) {
      return DIFF_HOST;
    }
    return opposite.process_id() == attr_.process_id() ? SAME_PROC : DIFF_PROC;
  }

  void ReconcileInitial() {
    for (const auto& reader : topology::TopologyManager::Instance()->GetReaders(
             attr_.channel_name())) {
      ApplyOpposite(reader, true);
    }
  }

  void OnTopologyChange(const proto::ChangeMsg& msg) {
    if (!msg.has_role_type() || msg.role_type() != proto::ROLE_READER ||
        !msg.has_role_attr() || !msg.role_attr().has_channel_name()) {
      return;
    }
    if (msg.operate_type() == proto::OPT_JOIN) {
      ApplyOpposite(msg.role_attr(), true);
    } else if (msg.operate_type() == proto::OPT_LEAVE) {
      ApplyOpposite(msg.role_attr(), false);
    }
  }

  void ApplyOpposite(const RoleAttributes& opposite, bool join) {
    const Relation relation = RelationFor(opposite);
    if (relation != SAME_PROC && relation != DIFF_PROC) return;
    const uint64_t id = RoleId(opposite);
    std::lock_guard<std::mutex> lock(mutex_);
    if (join) {
      if (opposite_.emplace(id, relation).second) {
        ReconcileBackendsLocked();
      }
      return;
    }
    if (opposite_.erase(id) != 0) ReconcileBackendsLocked();
  }

  void ReconcileBackendsLocked() {
    bool same_proc = false;
    bool diff_proc = false;
    for (const auto& item : opposite_) {
      same_proc = same_proc || item.second == SAME_PROC;
      diff_proc = diff_proc || item.second == DIFF_PROC;
    }
    if (!this->enabled_) {
      intra_enabled_ = false;
      shm_enabled_ = false;
      return;
    }
    if (same_proc != intra_enabled_) {
      intra_enabled_ = same_proc;
      if (intra_enabled_) intra_->Enable();
      else intra_->Disable();
    }
    if (diff_proc != shm_enabled_) {
      shm_enabled_ = diff_proc;
      if (shm_enabled_) shm_->Enable();
      else shm_->Disable();
    }
  }

  RoleAttributes attr_;
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, Relation> opposite_;
  std::shared_ptr<IntraTransmitter<M>> intra_;
  std::shared_ptr<ShmTransmitter<M>> shm_;
  topology::TopologyManager::ChangeConnection listener_;
  bool intra_enabled_ = false;
  bool shm_enabled_ = false;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_TRANSMITTER_HYBRID_TRANSMITTER_H_
