#ifndef MINICYBER_TRANSPORT_RECEIVER_HYBRID_RECEIVER_H_
#define MINICYBER_TRANSPORT_RECEIVER_HYBRID_RECEIVER_H_

#include <atomic>
#include <mutex>
#include <type_traits>
#include <unordered_map>

#include <google/protobuf/message.h>

#include "minicyber/common/types.h"
#include "minicyber/proto/role_attributes.pb.h"
#include "minicyber/proto/topology_change.pb.h"
#include "minicyber/topology/topology_manager.h"
#include "minicyber/transport/receiver/intra_receiver.h"
#include "minicyber/transport/receiver/receiver.h"
#include "minicyber/transport/receiver/shm_receiver.h"

namespace minicyber {
namespace transport {

// HybridReceiver 对齐 CyberRT 的按对端连接生命周期。一个本进程 writer
// 集合共享 INTRA 订阅，一个跨进程 writer 集合共享 SHM 订阅；两者同时存在
// 时分别只注册一次，因此同一消息不会因为 Hybrid 产生重复回调。
template <typename M>
class HybridReceiver : public Receiver<M> {
  static_assert(std::is_base_of<google::protobuf::Message, M>::value,
                "HybridTransport messages must derive from google::protobuf::Message");

 public:
  using Base = Receiver<M>;
  using MessageListener = typename Base::MessageListener;
  using RoleAttributes = proto::RoleAttributes;

  HybridReceiver(const RoleAttributes& attr,
                 const MessageListener& msg_listener)
      : Base(attr.channel_id(), msg_listener), attr_(attr) {
    intra_ = std::make_shared<IntraReceiver<M>>(
        this->channel_id_, [this](const std::shared_ptr<M>& msg) {
          if (accepting_.load(std::memory_order_acquire)) this->OnNewMessage(msg);
        });
    shm_ = std::make_shared<ShmReceiver<M>>(
        this->channel_id_, [this](const std::shared_ptr<M>& msg) {
          if (accepting_.load(std::memory_order_acquire)) this->OnNewMessage(msg);
        });
    listener_ = topology::TopologyManager::Instance()->AddChangeListener(
        [this](const proto::ChangeMsg& msg) { OnTopologyChange(msg); });
    ReconcileInitial();
  }

  ~HybridReceiver() override {
    topology::TopologyManager::Instance()->RemoveChangeListener(listener_);
    Disable();
  }

  void Enable() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (this->enabled_) return;
      this->enabled_ = true;
      accepting_.store(true, std::memory_order_release);
      UpdateDesiredBackendsLocked();
    }
    ReconcileBackends();
  }

  void Disable() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!this->enabled_) return;
      this->enabled_ = false;
      accepting_.store(false, std::memory_order_release);
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

  bool HasWriter() const {
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
    for (const auto& writer : topology::TopologyManager::Instance()->GetWriters(
             attr_.channel_name())) {
      ApplyOpposite(writer, true);
    }
  }

  void OnTopologyChange(const proto::ChangeMsg& msg) {
    if (!msg.has_role_type() || msg.role_type() != proto::ROLE_WRITER ||
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
      // 后端 Disable 可能等待在途回调，必须在 Hybrid 状态锁外执行；回调内
      // 再次关闭端点时只更新期望状态，由当前协调循环在回调退出后收口。
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
  std::shared_ptr<IntraReceiver<M>> intra_;
  std::shared_ptr<ShmReceiver<M>> shm_;
  topology::TopologyManager::ChangeConnection listener_;
  std::atomic<bool> reconciling_{false};
  std::atomic<bool> accepting_{false};
  bool intra_desired_ = false;
  bool shm_desired_ = false;
  bool intra_applied_ = false;
  bool shm_applied_ = false;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_RECEIVER_HYBRID_RECEIVER_H_
