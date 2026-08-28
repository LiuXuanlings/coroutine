#ifndef MINICYBER_DATA_DATA_DISPATCHER_H_
#define MINICYBER_DATA_DATA_DISPATCHER_H_

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "minicyber/data/channel_buffer.h"
#include "minicyber/data/data_notifier.h"

namespace minicyber {
namespace data {

// Routes a published message to every ChannelBuffer registered on a channel,
// then fires DataNotifier so waiting coroutines wake up.
//
// Buffer ownership is weak: the dispatcher holds weak_ptr to each CacheBuffer.
// A caller may also explicitly unregister its buffer during teardown.
//
// Lock strategy:
//   - registration and removal update the channel table under buffers_mtx_;
//   - Dispatch takes a vector snapshot under that lock, then releases it
//     before filling buffers or running notifier callbacks.
// This keeps buffer-vector lifetime safe while allowing callback re-entry.
template <typename T>
class DataDispatcher {
 public:
  using BufferType = CacheBuffer<std::shared_ptr<T>>;
  struct BufferEntry {
    std::weak_ptr<BufferType> buffer;
    bool receive_shm = false;
  };
  using BufferVector = std::vector<BufferEntry>;

  static DataDispatcher<T>* Instance() {
    static DataDispatcher<T> inst;
    return &inst;
  }

  // 只有 DataVisitor 注册 receive_shm。SHM 已由 ShmReceiver 单独回调普通
  // Reader；若再次唤醒 IntraReceiver 的 DataDispatcher 订阅会把同一消息
  // 错投为 INTRA 副本并造成 Hybrid 重复投递。
  void AddBuffer(const ChannelBuffer<T>& channel_buffer, bool receive_shm = false) {
    auto buffer = channel_buffer.Buffer();
    std::lock_guard<std::mutex> lock(buffers_mtx_);
    auto& buffers = buffers_map_[channel_buffer.channel_id()];
    buffers.erase(std::remove_if(buffers.begin(), buffers.end(),
                                 [](const BufferEntry& item) {
                                   return item.buffer.expired();
                                 }),
                  buffers.end());
    buffers.push_back({std::move(buffer), receive_shm});
  }

  bool RemoveBuffer(const ChannelBuffer<T>& channel_buffer) {
    const auto buffer = channel_buffer.Buffer();
    std::lock_guard<std::mutex> lock(buffers_mtx_);
    auto channel = buffers_map_.find(channel_buffer.channel_id());
    if (channel == buffers_map_.end()) {
      return false;
    }
    auto& buffers = channel->second;
    bool removed = false;
    buffers.erase(std::remove_if(buffers.begin(), buffers.end(),
                                 [&buffer, &removed](const BufferEntry& item) {
                                   auto registered = item.buffer.lock();
                                   if (registered == buffer) {
                                     removed = true;
                                     return true;
                                   }
                                   return !registered;
                                 }),
                  buffers.end());
    if (buffers.empty()) {
      buffers_map_.erase(channel);
    }
    return removed;
  }

  bool Dispatch(uint64_t channel_id, const std::shared_ptr<T>& msg) {
    return DispatchImpl(channel_id, msg, false);
  }

  // SHM 解码后的消息仅交给 Component 的 DataVisitor。常规 Reader 仍由
  // ShmReceiver::OnNewMessage 收到一次回调，不能经 Intra 数据总线回流。
  bool DispatchFromShm(uint64_t channel_id, const std::shared_ptr<T>& msg) {
    return DispatchImpl(channel_id, msg, true);
  }

 private:
  bool DispatchImpl(uint64_t channel_id, const std::shared_ptr<T>& msg,
                    bool shm_only) {
    BufferVector snapshot;
    {
      std::lock_guard<std::mutex> lock(buffers_mtx_);
      auto channel = buffers_map_.find(channel_id);
      if (channel == buffers_map_.end()) {
        return false;
      }
      snapshot = channel->second;
    }

    bool dispatched = false;
    for (auto& entry : snapshot) {
      if (shm_only && !entry.receive_shm) {
        continue;
      }
      if (auto buffer = entry.buffer.lock()) {
        std::lock_guard<std::mutex> lg(buffer->Mutex());
        buffer->Fill(msg);
        dispatched = true;
      }
    }
    if (!dispatched) {
      return false;
    }
    return shm_only ? notifier_->NotifyFromShm(channel_id)
                    : notifier_->Notify(channel_id);
  }

  DataDispatcher() = default;
  ~DataDispatcher() = default;
  DataDispatcher(const DataDispatcher&) = delete;
  DataDispatcher& operator=(const DataDispatcher&) = delete;

  DataNotifier* notifier_ = DataNotifier::Instance();
  std::mutex buffers_mtx_;
  std::unordered_map<uint64_t, BufferVector> buffers_map_;
};

}  // namespace data
}  // namespace minicyber

#endif  // MINICYBER_DATA_DATA_DISPATCHER_H_
