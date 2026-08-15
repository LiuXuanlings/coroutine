#ifndef MINICYBER_TRANSPORT_TRANSMITTER_HYBRID_TRANSMITTER_H_
#define MINICYBER_TRANSPORT_TRANSMITTER_HYBRID_TRANSMITTER_H_

#include <atomic>
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
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (this->enabled_) return;
      this->enabled_ = true;
      UpdateDesiredBackendsLocked();
    }
    ReconcileBackends();
  }

  void Disable() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!this->enabled_) return;
      this->enabled_ = false;
      UpdateDesiredBackendsLocked();
    }
    ReconcileBackends();
  }

  void Enable(const RoleAttributes& opposite_attr) {
    ApplyOpposite(opposite_attr, true);
  }

  void Disable(const RoleAttributes& opposite_attr) {
    ApplyOpposite(opposite_attr, false);
  }

  bool Transmit(const MessagePtr& msg) override {
    bool use_intra = false;
    bool use_shm = false;
    bool no_opposite = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!this->enabled_ || msg == nullptr) return false;
      use_intra = intra_desired_;
      use_shm = shm_desired_;
      no_opposite = opposite_.empty();
    }
    // INTRA 会在当前发布线程同步进入用户回调。这里不能持有 Hybrid 状态锁，
    // 否则回调内关闭 Writer/Node 会回入 Disable 并自死锁。
    bool delivered = false;
    if (use_intra) delivered = intra_->Transmit(msg) || delivered;
    if (use_shm) delivered = shm_->Transmit(msg) || delivered;
    // CyberRT 的 HybridTransmitter 对“无当前对端”也保持发布接口成功；
    // 路由是否有读者由 HasReader 在 Node 层观察。
    return delivered || no_opposite;
  }

  bool HasReader() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !opposite_.empty();
  }

  // 这是业务验收读取的已应用后端快照，不参与路由决策。只有对应的
  // ReconcileBackends 已完成 Enable 后才为真，用于证明同一个 Writer 同时
  // 保留 CyberRT per-opposite 的 INTRA 与 SHM 扇出，而非把 Audit 二次发布
  // 误当作远端数据面。
  bool IntraBackendEnabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return intra_applied_;
  }

  bool ShmBackendEnabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shm_applied_;
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
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (join && opposite_.emplace(id, relation).second) {
        UpdateDesiredBackendsLocked();
      } else if (!join && opposite_.erase(id) != 0) {
        UpdateDesiredBackendsLocked();
      }
    }
    ReconcileBackends();
  }

  void UpdateDesiredBackendsLocked() {
    bool same_proc = false;
    bool diff_proc = false;
    for (const auto& item : opposite_) {
      same_proc = same_proc || item.second == SAME_PROC;
      diff_proc = diff_proc || item.second == DIFF_PROC;
    }
    if (!this->enabled_) {
      intra_desired_ = false;
      shm_desired_ = false;
      return;
    }
    intra_desired_ = same_proc;
    shm_desired_ = diff_proc;
  }

  void ReconcileBackends() {
    bool expected = false;
    if (!reconciling_.compare_exchange_strong(expected, true)) return;
    for (;;) {
      enum class Action {
        NONE,
        ENABLE_INTRA,
        DISABLE_INTRA,
        ENABLE_SHM,
        DISABLE_SHM
      };
      Action action = Action::NONE;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (intra_applied_ != intra_desired_) {
          intra_applied_ = intra_desired_;
          action = intra_applied_ ? Action::ENABLE_INTRA : Action::DISABLE_INTRA;
        } else if (shm_applied_ != shm_desired_) {
          shm_applied_ = shm_desired_;
          action = shm_applied_ ? Action::ENABLE_SHM : Action::DISABLE_SHM;
        }
      }
      if (action == Action::ENABLE_INTRA) intra_->Enable();
      else if (action == Action::DISABLE_INTRA) intra_->Disable();
      else if (action == Action::ENABLE_SHM) shm_->Enable();
      else if (action == Action::DISABLE_SHM) shm_->Disable();
      else {
        reconciling_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex_);
        if (intra_applied_ == intra_desired_ && shm_applied_ == shm_desired_) return;
        expected = false;
        if (!reconciling_.compare_exchange_strong(expected, true)) return;
      }
    }
  }

  RoleAttributes attr_;
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, Relation> opposite_;
  std::shared_ptr<IntraTransmitter<M>> intra_;
  std::shared_ptr<ShmTransmitter<M>> shm_;
  topology::TopologyManager::ChangeConnection listener_;
  std::atomic<bool> reconciling_{false};
  bool intra_desired_ = false;
  bool shm_desired_ = false;
  bool intra_applied_ = false;
  bool shm_applied_ = false;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_TRANSMITTER_HYBRID_TRANSMITTER_H_
