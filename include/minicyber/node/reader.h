#ifndef MINICYBER_NODE_READER_H_
#define MINICYBER_NODE_READER_H_

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

#include <google/protobuf/message.h>

#include "minicyber/proto/role_attributes.pb.h"
#include "minicyber/topology/topology_manager.h"
#include "minicyber/transport/transport.h"

namespace minicyber {
namespace node {

// Reader 保留业务需要的有界 Observe 队列，但接收回调只入队和通知用户；
// 它不在发布线程同步执行业务 Proc。/ 将在此边界之上接入
// DataVisitor 与 RoutineFactory。
template <typename T>
class Reader {
  static_assert(std::is_base_of<google::protobuf::Message, T>::value,
                "Node Channel messages must derive from google::protobuf::Message");

 public:
  using CallbackFunc = std::function<void(const std::shared_ptr<T>&)>;
  static constexpr uint32_t kDefaultPendingQueueSize = 1;

  Reader(proto::RoleAttributes role_attr, const CallbackFunc& callback = nullptr,
         uint32_t pending_queue_size = kDefaultPendingQueueSize)
      : role_attr_(std::move(role_attr)),
        callback_(callback),
        pending_queue_size_(pending_queue_size == 0 ? 1 : pending_queue_size) {}
  ~Reader() { Shutdown(); }

  bool Init() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (init_) return true;
    receiver_ = transport::Transport::CreateHybridReceiver<T>(
        role_attr_, [this](const std::shared_ptr<T>& message) {
          Enqueue(message);
          if (callback_) callback_(message);
        });
    if (receiver_ == nullptr ||
        !topology::TopologyManager::Instance()->Join(proto::ROLE_READER,
                                                      role_attr_)) {
      if (receiver_) receiver_->Disable();
      receiver_.reset();
      return false;
    }
    init_ = true;
    return true;
  }

  void Shutdown() {
    std::shared_ptr<transport::HybridReceiver<T>> receiver;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      if (!init_) return;
      init_ = false;
      receiver = std::move(receiver_);
    }
    if (receiver) receiver->Disable();
    topology::TopologyManager::Instance()->Leave(proto::ROLE_READER, role_attr_);
  }

  bool IsInit() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return init_;
  }
  bool HasWriter() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return init_ && topology::TopologyManager::Instance()->HasWriter(
                        role_attr_.channel_name());
  }
  const std::string& channel() const { return role_attr_.channel_name(); }
  const proto::RoleAttributes& role_attr() const { return role_attr_; }

  void Observe() {
    std::lock_guard<std::mutex> lock(history_mutex_);
    observed_history_ = pending_history_;
    pending_history_.clear();
  }

  void ClearData() {
    std::lock_guard<std::mutex> lock(history_mutex_);
    pending_history_.clear();
    observed_history_.clear();
    has_received_ = false;
  }

  bool HasReceived() const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    return has_received_;
  }
  bool Empty() const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    return observed_history_.empty();
  }
  uint32_t PendingQueueSize() const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    return pending_queue_size_;
  }
  void SetHistoryDepth(uint32_t depth) {
    std::lock_guard<std::mutex> lock(history_mutex_);
    pending_queue_size_ = depth == 0 ? 1 : depth;
    TrimHistory(&pending_history_);
    TrimHistory(&observed_history_);
  }
  uint32_t GetHistoryDepth() const { return PendingQueueSize(); }
  std::shared_ptr<T> GetLatestObserved() const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    return observed_history_.empty() ? nullptr : observed_history_.back();
  }
  std::shared_ptr<T> GetOldestObserved() const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    return observed_history_.empty() ? nullptr : observed_history_.front();
  }

 private:
  void Enqueue(const std::shared_ptr<T>& message) {
    if (message == nullptr) return;
    std::lock_guard<std::mutex> lock(history_mutex_);
    has_received_ = true;
    pending_history_.push_back(message);
    TrimHistory(&pending_history_);
  }
  void TrimHistory(std::deque<std::shared_ptr<T>>* history) const {
    while (history->size() > pending_queue_size_) history->pop_front();
  }

  proto::RoleAttributes role_attr_;
  CallbackFunc callback_;
  mutable std::mutex lifecycle_mutex_;
  std::shared_ptr<transport::HybridReceiver<T>> receiver_;
  mutable std::mutex history_mutex_;
  std::deque<std::shared_ptr<T>> pending_history_;
  std::deque<std::shared_ptr<T>> observed_history_;
  uint32_t pending_queue_size_;
  bool has_received_ = false;
  bool init_ = false;
};

}  // namespace node
}  // namespace minicyber

#endif  // MINICYBER_NODE_READER_H_
