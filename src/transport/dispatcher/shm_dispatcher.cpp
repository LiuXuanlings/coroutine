#include "minicyber/transport/dispatcher/shm_dispatcher.h"

#include <cstring>
#include <utility>
#include <vector>

namespace minicyber {
namespace transport {

namespace {
thread_local const void* g_executing_listener = nullptr;
}

ShmDispatcher::ShmDispatcher() { Init(); }

ShmDispatcher::~ShmDispatcher() { Shutdown(); }

void ShmDispatcher::Init() {
  notifier_ = std::make_unique<ConditionNotifier>();
  if (!notifier_->Init()) {
    return;
  }
  running_.store(true);
  thread_ = std::thread(&ShmDispatcher::ThreadFunc, this);
}

void ShmDispatcher::AddSegment(uint64_t channel_id) {
  std::lock_guard<std::mutex> lock(segments_mutex_);
  if (!running_.load(std::memory_order_acquire)) return;
  if (segments_.count(channel_id) > 0) return;
  auto seg = std::make_shared<PosixSegment>(channel_id);
  if (!seg->Open()) {
    return;
  }
  segments_[channel_id] = std::move(seg);
}

uint64_t ShmDispatcher::AddListener(uint64_t channel_id,
                                     RawMessageListener callback) {
  if (!callback) return 0;
  const uint64_t id = next_listener_id_.fetch_add(1, std::memory_order_relaxed);
  auto listener = std::make_shared<RawListener>(std::move(callback));
  AddSegment(channel_id);
  {
    std::lock_guard<std::mutex> lock(listeners_mutex_);
    listeners_[channel_id].emplace(id, std::move(listener));
  }
  return id;
}

void ShmDispatcher::RemoveListener(uint64_t channel_id, uint64_t listener_id) {
  std::shared_ptr<RawListener> listener;
  {
    std::lock_guard<std::mutex> lock(listeners_mutex_);
    const auto channel = listeners_.find(channel_id);
    if (channel == listeners_.end()) return;
    const auto item = channel->second.find(listener_id);
    if (item == channel->second.end()) return;
    listener = item->second;
    channel->second.erase(item);
    if (channel->second.empty()) listeners_.erase(channel);
  }
  std::unique_lock<std::mutex> lock(listener->mutex);
  listener->active = false;
  if (g_executing_listener != listener.get()) {
    listener->idle.wait(lock, [&listener] { return listener->in_flight == 0; });
  }
}

void ShmDispatcher::ThreadFunc() {
  ReadableInfo info;
  while (running_.load()) {
    // 原生 ConditionNotifier：Listen 返回 ReadableInfo，直接给出目标
    // channel_id 与 block_index，无需扫描所有 segment
    if (!notifier_->Listen(100, &info)) {
      continue;
    }
    // host_id 过滤暂不启用（单机测试场景）
    ReadMessage(info.channel_id, info.block_index);
  }
}

void ShmDispatcher::ReadMessage(uint64_t channel_id, uint32_t block_index) {
  std::shared_ptr<PosixSegment> seg;
  {
    std::lock_guard<std::mutex> lock(segments_mutex_);
    auto it = segments_.find(channel_id);
    if (it == segments_.end()) return;
    seg = it->second;
  }
  if (!seg) return;

  ShmReadableBlock rb;
  if (!seg->AcquireBlockToRead(block_index, &rb)) return;

  uint64_t msg_size = rb.block->msg_size();
  // 空 Protobuf 的合法序列化结果是零字节；它仍须作为一条消息分发，不能
  // 因沿用字符串负载的非空判断而被静默丢弃。
  if (msg_size <= seg->block_buf_size()) {
    std::string msg(reinterpret_cast<const char*>(rb.buf),
                    static_cast<size_t>(msg_size));
    auto msg_ptr = std::make_shared<std::string>(std::move(msg));
    data::DataDispatcher<std::string>::Instance()->Dispatch(channel_id, msg_ptr);
    NotifyRawListeners(channel_id, *msg_ptr);
  }
  seg->ReleaseReadBlock(rb);
}

void ShmDispatcher::NotifyRawListeners(uint64_t channel_id,
                                        const std::string& payload) {
  std::vector<std::shared_ptr<RawListener>> snapshot;
  {
    std::lock_guard<std::mutex> lock(listeners_mutex_);
    const auto channel = listeners_.find(channel_id);
    if (channel == listeners_.end()) return;
    snapshot.reserve(channel->second.size());
    for (const auto& item : channel->second) {
      auto listener = item.second;
      std::lock_guard<std::mutex> listener_lock(listener->mutex);
      if (listener->active) {
        ++listener->in_flight;
        snapshot.push_back(std::move(listener));
      }
    }
  }
  for (const auto& listener : snapshot) {
    RawMessageListener callback;
    {
      std::lock_guard<std::mutex> lock(listener->mutex);
      callback = listener->callback;
    }
    g_executing_listener = listener.get();
    try {
      callback(payload);
    } catch (...) {
      // 监听器异常不能终止 dispatcher 线程，也不能跳过 in-flight 归还。
    }
    g_executing_listener = nullptr;
    {
      std::lock_guard<std::mutex> lock(listener->mutex);
      if (listener->in_flight > 0) --listener->in_flight;
      if (listener->in_flight == 0) listener->idle.notify_all();
    }
  }
}

void ShmDispatcher::Shutdown() {
  running_.exchange(false);
  if (thread_.joinable()) thread_.join();
  if (notifier_) notifier_->Shutdown();
  std::lock_guard<std::mutex> lock(segments_mutex_);
  segments_.clear();
  std::vector<std::shared_ptr<RawListener>> listeners;
  {
    std::lock_guard<std::mutex> listener_lock(listeners_mutex_);
    for (auto& channel : listeners_) {
      for (auto& item : channel.second) listeners.push_back(item.second);
    }
    listeners_.clear();
  }
  for (const auto& listener : listeners) {
    std::unique_lock<std::mutex> listener_lock(listener->mutex);
    listener->active = false;
    if (g_executing_listener != listener.get()) {
      listener->idle.wait(listener_lock,
                          [&listener] { return listener->in_flight == 0; });
    }
  }
}

}  // namespace transport
}  // namespace minicyber
