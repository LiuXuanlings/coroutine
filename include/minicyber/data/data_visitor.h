#ifndef MINICYBER_DATA_DATA_VISITOR_H_
#define MINICYBER_DATA_DATA_VISITOR_H_

#include <memory>

#include "minicyber/common/types.h"
#include "minicyber/croutine/croutine.h"
#include "minicyber/data/channel_buffer.h"
#include "minicyber/data/data_dispatcher.h"
#include "minicyber/data/data_visitor_base.h"
#include "minicyber/data/fusion/all_latest.h"

namespace minicyber {
namespace data {

struct VisitorConfig {
  VisitorConfig(uint64_t id, uint32_t size) : channel_id(id), queue_size(size) {}
  uint64_t channel_id;
  uint32_t queue_size;
};

// =============================================================================
// DataVisitor：协程侧的数据访问器。
//
// 模板形参对齐 CyberRT：M0 为主通道（驱动轴），M1/M2/M3 为副通道。
// 通过偏特化提供 1/2/3/4 通道版本。MiniCyber 当前实现 1 通道与 2 通道版本；
// 3/4 通道保持未定义（调用会触发编译期 static_assert 或链接错误，按需扩展）。
//
// 单通道版本：直接从 ChannelBuffer 取数据。
// 双通道版本：组合 AllLatest 融合引擎，仅当主通道数据到达且副通道有最新值时
//   才产生一组对齐的融合数据。DataNotifier 仅挂在主通道——副通道到达不会
//   单独唤醒协程，符合 AllLatest "主通道驱动" 语义。
// =============================================================================

// Forward declarations of the partial specializations (defined below).
template <typename M0, typename M1 = NullType, typename M2 = NullType,
          typename M3 = NullType>
class DataVisitor;  // primary, intentionally undefined for the general case

// -----------------------------------------------------------------------------
// Single-channel specialization: DataVisitor<M0, NullType, NullType, NullType>
// Behaves identically to the pre-Step-35 DataVisitor<T>.
// -----------------------------------------------------------------------------
template <typename M0>
class DataVisitor<M0, NullType, NullType, NullType> : public DataVisitorBase {
 public:
  using BufferType = CacheBuffer<std::shared_ptr<M0>>;

  explicit DataVisitor(const VisitorConfig& config)
      : buffer_(config.channel_id,
                std::make_shared<BufferType>(config.queue_size)) {
    DataDispatcher<M0>::Instance()->AddBuffer(buffer_);
    data_notifier_->AddNotifier(buffer_.channel_id(), notifier_);
  }

  // Non-blocking fetch. Returns false when caught up.
  bool TryFetch(std::shared_ptr<M0>& m) {
    if (buffer_.Fetch(&next_msg_index_, m)) {
      ++next_msg_index_;
      return true;
    }
    return false;
  }

  // Blocking fetch for use inside a CRoutine. Yields with DATA_WAIT when no
  // data is available; retries after the notifier fires.
  bool Fetch(std::shared_ptr<M0>& m) {
    while (!TryFetch(m)) {
      auto rt = CRoutine::GetThis();
      rt->SetState(RoutineState::DATA_WAIT);
      CRoutine::Yield(RoutineState::DATA_WAIT);
    }
    return true;
  }

  uint64_t channel_id() const { return buffer_.channel_id(); }
  const ChannelBuffer<M0>& buffer() const { return buffer_; }

 private:
  ChannelBuffer<M0> buffer_;
};

// -----------------------------------------------------------------------------
// Two-channel specialization: DataVisitor<M0, M1, NullType, NullType>
// Integrates AllLatest fusion. Notifier is bound to the PRIMARY channel only.
//
// Construction order matters (Step 35 design decision):
//   1. Register both ChannelBuffers with their DataDispatcher.
//   2. Construct AllLatest — it registers ITS OWN notifier on M0 to fill the
//      fusion buffer when M0 data arrives.
//   3. Register the visitor's notifier on M0 AFTER AllLatest's notifier.
//      DataNotifier::Notify iterates notifiers in registration order, so the
//      fusion buffer is populated BEFORE the coroutine is woken. Reversing
//      this order would let the coroutine wake, see an empty fusion buffer,
//      and park — missing the wake until the next M0 dispatch.
// -----------------------------------------------------------------------------
template <typename M0, typename M1>
class DataVisitor<M0, M1, NullType, NullType> : public DataVisitorBase {
 public:
  using BufferType0 = CacheBuffer<std::shared_ptr<M0>>;
  using BufferType1 = CacheBuffer<std::shared_ptr<M1>>;

  DataVisitor(const VisitorConfig& cfg0, const VisitorConfig& cfg1)
      : buffer_m0_(cfg0.channel_id,
                   std::make_shared<BufferType0>(cfg0.queue_size)),
        buffer_m1_(cfg1.channel_id,
                   std::make_shared<BufferType1>(cfg1.queue_size)) {
    DataDispatcher<M0>::Instance()->AddBuffer(buffer_m0_);
    DataDispatcher<M1>::Instance()->AddBuffer(buffer_m1_);
    // Step 1: AllLatest registers its M0 notifier (fills fusion buffer).
    data_fusion_ = new fusion::AllLatest<M0, M1>(buffer_m0_, buffer_m1_);
    // Step 2: visitor's wake-up notifier on M0, registered AFTER AllLatest's
    // so the fusion buffer is populated before the coroutine re-checks.
    data_notifier_->AddNotifier(buffer_m0_.channel_id(), notifier_);
  }

  ~DataVisitor() {
    if (data_fusion_) {
      delete data_fusion_;
      data_fusion_ = nullptr;
    }
  }

  // Non-blocking fusion fetch. Returns true and fills m0/m1 with the next
  // aligned pair. Returns false when the fusion buffer is empty or the
  // consumer has caught up.
  //
  // Cursor advancement is owned by AllLatest::Fusion (it calls ++(*index)
  // internally), so unlike the single-channel TryFetch we do NOT bump
  // next_msg_index_ here — that would double-advance and read past Tail.
  bool TryFetch(std::shared_ptr<M0>& m0, std::shared_ptr<M1>& m1) {
    return data_fusion_->Fusion(&next_msg_index_, m0, m1);
  }

  // Blocking fusion fetch for use inside a CRoutine. Parks in DATA_WAIT until
  // a fused pair is available. Wakes on M0 dispatch only (M1 dispatch does
  // not trigger the notifier); the coroutine re-checks and parks again if the
  // fusion buffer is still empty (e.g. M1 had no data at the time of M0's
  // dispatch).
  bool Fetch(std::shared_ptr<M0>& m0, std::shared_ptr<M1>& m1) {
    while (!TryFetch(m0, m1)) {
      auto rt = CRoutine::GetThis();
      rt->SetState(RoutineState::DATA_WAIT);
      CRoutine::Yield(RoutineState::DATA_WAIT);
    }
    return true;
  }

  // Primary channel is the "drive shaft" — it's the channel the notifier
  // binds to and the one whose dispatch triggers fusion attempts.
  uint64_t channel_id() const { return buffer_m0_.channel_id(); }
  uint64_t primary_channel_id() const { return buffer_m0_.channel_id(); }
  uint64_t secondary_channel_id() const { return buffer_m1_.channel_id(); }

  const ChannelBuffer<M0>& primary_buffer() const { return buffer_m0_; }
  const ChannelBuffer<M1>& secondary_buffer() const { return buffer_m1_; }

 private:
  ChannelBuffer<M0> buffer_m0_;
  ChannelBuffer<M1> buffer_m1_;
  fusion::AllLatest<M0, M1>* data_fusion_ = nullptr;
};

}  // namespace data
}  // namespace minicyber

#endif  // MINICYBER_DATA_DATA_VISITOR_H_
