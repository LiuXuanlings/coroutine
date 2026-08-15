#ifndef MINICYBER_NODE_READER_H_
#define MINICYBER_NODE_READER_H_

#include <functional>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "minicyber/topology/topology_manager.h"
#include "minicyber/transport/receiver/receiver.h"
#include "minicyber/transport/transport.h"

namespace minicyber {
namespace node {

// =============================================================================
// Reader：订阅端用户接口（对齐 CyberRT Reader<M>）
//
// 职责：封装 Receiver，提供回调式消息接收 API。
//   创建时自动向 TopologyManager 注册为 reader，使 Transport 路由可感知拓扑。
//
// 生命周期：
//   Init()    : 注册拓扑 + Transport::CreateReceiver<T>
//   Shutdown(): receiver_->Disable()
//
// 回调机制：
//   用户传入 CallbackFunc，Reader 内部传给 Transport::CreateReceiver，
//   由底层 Receiver 在数据到达时调用。
//
// 与 CyberRT 的简化：
//   - 去掉 RoleAttributes / ReaderBase / Blocker / DataVisitor / 协程任务创建
//   - 去掉 HasWriter / GetWriters / 动态拓扑监听
//   - 保留有界历史（默认深度 1）；去掉协程 Blocker 与动态拓扑监听
//   - 拓扑在 Init 时静态注册，不做动态变更通知
// =============================================================================

template <typename T>
class Reader {
 public:
  using CallbackFunc = std::function<void(const std::shared_ptr<T>&)>;
  static constexpr uint32_t kDefaultPendingQueueSize = 1;

  Reader(const std::string& node_name, const std::string& channel,
         const CallbackFunc& callback = nullptr,
         uint32_t pending_queue_size = kDefaultPendingQueueSize)
      : node_name_(node_name),
        channel_(channel),
        callback_(callback),
        pending_queue_size_(pending_queue_size == 0 ? 1 : pending_queue_size) {}

  ~Reader() { Shutdown(); }

  // 注册拓扑 + 创建底层 Receiver
  void Init() {
    if (init_) return;
    topology::TopologyManager::Instance()->AddChannelReader(
        channel_, node_name_, ::getpid());
    receiver_ = transport::Transport::CreateReceiver<T>(
        channel_, [this](const std::shared_ptr<T>& message) {
          Enqueue(message);
          if (callback_) callback_(message);
        });
    init_ = true;
  }

  void Shutdown() {
    if (!init_) return;
    init_ = false;
    if (receiver_) {
      receiver_->Disable();
      receiver_.reset();
    }
  }

  bool IsInit() const { return init_; }
  const std::string& channel() const { return channel_; }

  // 将最近一次接收窗口固定为当前 publish 队列，供无回调消费方读取。
  void Observe() {
    std::lock_guard<std::mutex> lock(history_mutex_);
    observed_history_ = pending_history_;
    pending_history_.clear();
  }

  void ClearData() {
    std::lock_guard<std::mutex> lock(history_mutex_);
    pending_history_.clear();
    observed_history_.clear();
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

  std::string node_name_;
  std::string channel_;
  CallbackFunc callback_;
  std::shared_ptr<transport::Receiver<T>> receiver_;
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
