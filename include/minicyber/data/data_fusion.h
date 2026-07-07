#ifndef MINICYBER_DATA_DATA_FUSION_H_
#define MINICYBER_DATA_DATA_FUSION_H_

#include <cstdint>
#include <memory>

#include "minicyber/croutine/croutine.h"
#include "minicyber/data/data_visitor.h"

namespace minicyber {
namespace data {

// Two-channel barrier fusion ("AllLatest" semantics). Wraps two
// DataVisitor<T> instances and only reports readiness when BOTH channels
// have at least one message available. Each successful fuse returns the
// newest of each channel.
//
// Design notes:
//   - Source-channel cursors are NOT advanced. Instead we track the Tail
//     of each source buffer at the time of the last successful fuse. A new
//     fuse is produced only when at least one channel's Tail has advanced
//     since last time — so repeated calls without new data return false
//     (caught up), matching AllLatest without a separate fusion buffer.
//   - Reuses DataVisitor for ChannelBuffer + DataDispatcher + DataNotifier
//     integration. Wake-up is a polling barrier: the coroutine wakes when
//     EITHER channel fires its notifier, re-checks, and parks again if only
//     one is ready.
//   - The caller binds the SAME wake callback to BOTH visitors via
//     RegisterWakeCallback() so a Dispatch on either channel re-enqueues
//     the parked coroutine.
template <typename T1, typename T2>
class DataFusion {
 public:
  DataFusion(const VisitorConfig& cfg1, const VisitorConfig& cfg2)
      : dv1_(cfg1), dv2_(cfg2) {}

  // Bind a single wake-up callback to BOTH underlying DataVisitors, so a
  // Dispatch on either channel re-enqueues the parked coroutine. The
  // Scheduler/Node layer passes something like
  //   [&]() { sched.NotifyTask(crid); }
  void RegisterWakeCallback(std::function<void()> callback) {
    auto cb = std::make_shared<std::function<void()>>(std::move(callback));
    dv1_.RegisterNotifyCallback([cb]() { (*cb)(); });
    dv2_.RegisterNotifyCallback([cb]() { (*cb)(); });
  }

  // Non-blocking: returns true only when BOTH channels have at least one
  // message AND at least one channel has new data since the last successful
  // fuse. On success, fills msg1/msg2 with the newest of each channel. On
  // failure, leaves both untouched.
  bool TryAllLatest(std::shared_ptr<T1>& msg1, std::shared_ptr<T2>& msg2) {
    // Access the underlying CacheBuffers directly. CacheBuffer methods (Tail,
    // Back, Empty) do NOT lock internally — locking is the caller's job, so
    // we lock both mutexes together and read under the combined lock.
    auto cb1 = dv1_.buffer().Buffer();
    auto cb2 = dv2_.buffer().Buffer();
    std::mutex& mtx1 = cb1->Mutex();
    std::mutex& mtx2 = cb2->Mutex();
    std::unique_lock<std::mutex> lk1(mtx1, std::defer_lock);
    std::unique_lock<std::mutex> lk2(mtx2, std::defer_lock);
    std::lock(lk1, lk2);

    if (cb1->Empty() || cb2->Empty()) {
      return false;
    }
    uint64_t tail1 = cb1->Tail();
    uint64_t tail2 = cb2->Tail();
    if (tail1 == last_fused_tail1_ && tail2 == last_fused_tail2_) {
      return false;  // no new data on either channel since last fuse
    }
    last_fused_tail1_ = tail1;
    last_fused_tail2_ = tail2;
    msg1 = cb1->Back();
    msg2 = cb2->Back();
    return true;
  }

  // Blocking version for use inside a CRoutine. Parks in DATA_WAIT until both
  // channels are ready; retries on each wake. Returns true once both messages
  // are fetched. MUST be called from a CRoutine context.
  bool WaitForAllLatest(std::shared_ptr<T1>& msg1, std::shared_ptr<T2>& msg2) {
    while (!TryAllLatest(msg1, msg2)) {
      auto rt = CRoutine::GetThis();
      rt->SetState(RoutineState::DATA_WAIT);
      CRoutine::Yield(RoutineState::DATA_WAIT);
    }
    return true;
  }

  uint64_t channel_id1() const { return dv1_.channel_id(); }
  uint64_t channel_id2() const { return dv2_.channel_id(); }

 private:
  DataVisitor<T1> dv1_;
  DataVisitor<T2> dv2_;
  // Tail of each source buffer at the time of the last successful fuse.
  // Initialized to a sentinel that no real Tail will match once data exists,
  // so the first fuse fires as soon as both channels have any data.
  uint64_t last_fused_tail1_ = 0;
  uint64_t last_fused_tail2_ = 0;
};

}  // namespace data
}  // namespace minicyber

#endif  // MINICYBER_DATA_DATA_FUSION_H_