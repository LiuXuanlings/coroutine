#ifndef MINICYBER_DATA_DATA_VISITOR_H_
#define MINICYBER_DATA_DATA_VISITOR_H_

#include <memory>

#include "minicyber/croutine/croutine.h"
#include "minicyber/data/channel_buffer.h"
#include "minicyber/data/data_dispatcher.h"
#include "minicyber/data/data_visitor_base.h"

namespace minicyber {
namespace data {

struct VisitorConfig {
  VisitorConfig(uint64_t id, uint32_t size) : channel_id(id), queue_size(size) {}
  uint64_t channel_id;
  uint32_t queue_size;
};

// Single-channel visitor. Owns a ChannelBuffer registered with DataDispatcher,
// and a Notifier registered with DataNotifier. Two fetch entry points:
//
//   TryFetch(m): non-blocking. Returns true and fills m with the next message
//               (advancing next_msg_index_). Returns false when caught up.
//               Pure primitive — does NOT touch coroutine state.
//
//   Fetch(m):    convenience wrapper for coroutine context. Loops: TryFetch;
//               on success returns; on failure sets the current CRoutine to
//               DATA_WAIT and Yields. When a writer Dispatches new data,
//               DataNotifier fires the bound callback (set via
//               RegisterNotifyCallback), which flips the routine's update
//               flag so the scheduler re-enqueues it; the loop then retries
//               TryFetch and succeeds.
template <typename T>
class DataVisitor : public DataVisitorBase {
 public:
  using BufferType = CacheBuffer<std::shared_ptr<T>>;

  explicit DataVisitor(const VisitorConfig& config)
      : buffer_(config.channel_id,
                std::make_shared<BufferType>(config.queue_size)) {
    DataDispatcher<T>::Instance()->AddBuffer(buffer_);
    data_notifier_->AddNotifier(buffer_.channel_id(), notifier_);
  }

  // Non-blocking fetch. Caller is responsible for setting DATA_WAIT/Yield
  // (see routine_factory pattern). Returns false when caught up.
  bool TryFetch(std::shared_ptr<T>& m) {  
    if (buffer_.Fetch(&next_msg_index_, m)) {
      ++next_msg_index_;
      return true;
    }
    return false;
  }

  // Blocking fetch for use inside a CRoutine. Yields with DATA_WAIT when no
  // data is available; retries after the notifier fires. Returns true once a
  // message is fetched. MUST be called from a CRoutine context (CRoutine::GetThis()
  // must be non-null).
  bool Fetch(std::shared_ptr<T>& m) { 
    while (!TryFetch(m)) {
      auto rt = CRoutine::GetThis();
      // SetUpdateFlag() is flipped by the notifier callback; Yield(DATA_WAIT)
      // parks the coroutine until data arrives.
      rt->SetState(RoutineState::DATA_WAIT);
      CRoutine::Yield(RoutineState::DATA_WAIT);
    }
    return true;
  }

  uint64_t channel_id() const { return buffer_.channel_id(); }
  const ChannelBuffer<T>& buffer() const { return buffer_; }

 private:
  ChannelBuffer<T> buffer_;
};

}  // namespace data
}  // namespace minicyber

#endif  // MINICYBER_DATA_DATA_VISITOR_H_