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
  using BufferVector = std::vector<std::weak_ptr<BufferType>>;

  static DataDispatcher<T>* Instance() {
    static DataDispatcher<T> inst;
    return &inst;
  }

  void AddBuffer(const ChannelBuffer<T>& channel_buffer) {
    auto buffer = channel_buffer.Buffer();
    std::lock_guard<std::mutex> lock(buffers_mtx_);
    auto& buffers = buffers_map_[channel_buffer.channel_id()];
    buffers.erase(std::remove_if(buffers.begin(), buffers.end(),
                                 [](const std::weak_ptr<BufferType>& item) {
                                   return item.expired();
                                 }),
                  buffers.end());
    buffers.emplace_back(std::move(buffer));
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
                                 [&buffer, &removed](const std::weak_ptr<BufferType>& item) {
                                   auto registered = item.lock();
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
    BufferVector snapshot;
    {
      std::lock_guard<std::mutex> lock(buffers_mtx_);
      auto channel = buffers_map_.find(channel_id);
      if (channel == buffers_map_.end()) {
        return false;
      }
      snapshot = channel->second;
    }

    for (auto& buffer_wptr : snapshot) {
      if (auto buffer = buffer_wptr.lock()) {
        std::lock_guard<std::mutex> lg(buffer->Mutex());
        buffer->Fill(msg);
      }
    }
    return notifier_->Notify(channel_id);
  }

 private:
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
