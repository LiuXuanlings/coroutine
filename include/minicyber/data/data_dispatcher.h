#ifndef MINICYBER_DATA_DATA_DISPATCHER_H_
#define MINICYBER_DATA_DATA_DISPATCHER_H_

#include <memory>
#include <mutex>
#include <vector>

#include "minicyber/base/atomic_hash_map.h"
#include "minicyber/data/channel_buffer.h"
#include "minicyber/data/data_notifier.h"

namespace minicyber {
namespace data {

// Routes a published message to every ChannelBuffer registered on a channel,
// then fires DataNotifier so waiting coroutines wake up.
//
// Buffer ownership is weak: the dispatcher holds weak_ptr to each CacheBuffer,
// so destroying a ChannelBuffer/DataVisitor simply leaves a dead entry that
// gets skipped on the next Dispatch.
//
// Lock strategy (Step 26, AtomicHashMap version):
//   - AddBuffer: copy-on-write on the map entry (atomic CAS).
//   - Dispatch:  read-only Get() from the map; each buffer is filled under
//                its own mutex. No map-level lock needed.
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
    uint64_t ch_id = channel_buffer.channel_id();
    BufferVector* existing = nullptr;
    if (buffers_map_.Get(ch_id, &existing)) {
      // Copy-on-write: atomically publish the extended vector.
      BufferVector copy = *existing;
      copy.emplace_back(std::move(buffer));
      buffers_map_.Set(ch_id, std::move(copy));
    } else {
      buffers_map_.Set(ch_id, BufferVector{std::move(buffer)});
    }
  }

  bool Dispatch(uint64_t channel_id, const std::shared_ptr<T>& msg) {
    BufferVector* buffers_ptr = nullptr;
    if (!buffers_map_.Get(channel_id, &buffers_ptr)) {
      return false;
    }
    // Take a snapshot (copy of weak_ptrs — cheap) so a callback that
    // re-enters Dispatch sees a consistent view.
    BufferVector snapshot = *buffers_ptr;

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
  AtomicHashMap<uint64_t, BufferVector, 256> buffers_map_;
};

}  // namespace data
}  // namespace minicyber

#endif  // MINICYBER_DATA_DATA_DISPATCHER_H_