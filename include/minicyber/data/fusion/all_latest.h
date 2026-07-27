#ifndef MINICYBER_DATA_FUSION_ALL_LATEST_H_
#define MINICYBER_DATA_FUSION_ALL_LATEST_H_

#include <memory>
#include <mutex>
#include <tuple>
#include <utility>

#include "minicyber/data/cache_buffer.h"
#include "minicyber/data/channel_buffer.h"
#include "minicyber/data/data_notifier.h"
#include "minicyber/data/fusion/data_fusion.h"

// =============================================================================
// AllLatest：主通道触发、拉取副通道最新值的融合策略
//   （对齐 CyberRT data::fusion::AllLatest）
//
// 原理：
//   1. 在构造时注册一个 DataNotifier 回调到 primary channel。
//   2. 每次 primary 有新数据到达（Dispatch → DataNotifier::Notify），
//      回调被触发：拉取 primary 最新值 + secondary 最新值 → 打包成元组
//      写入 fusion buffer。
//   3. Fusion() 从 fusion buffer 按 index 取出元组并解包。
//
// 核心语义：
//   - primary 是"驱动轴"：只有 primary 有数据时才尝试融合。
//   - secondary 是"随从"：fusion 时取 secondary 的最新值（可能跟上次
//     fusion 时的值相同，即"新的 primary + 老的 secondary"）。
//   - 这符合自动驾驶中最常见的"高频 + 低频"传感器融合场景。
//
// 与 CyberRT 的差异：
//   CyberRT 的 AllLatest 通过 CacheBuffer::SetFusionCallback 在 Fill
//   内部加锁的 callback 中执行融合。MiniCyber 的 CacheBuffer 无此钩子，
//   改用 DataNotifier 回调（Dispatch 后触发）。效果一致：primary 数据
//   到达 → 执行 Fusion → 填充 fusion buffer。区别是锁不再由 CacheBuffer
//   内部持有，融合回调需自己锁 fusion buffer 的 Mutex。
// =============================================================================

namespace minicyber {
namespace data {
namespace fusion {

template <typename M0, typename M1 = NullType>
class AllLatest : public DataFusion<M0, M1> {
  using FusionDataType = std::tuple<std::shared_ptr<M0>, std::shared_ptr<M1>>;

 public:
  /// 构造 AllLatest 融合器。
  /// @param buffer_0 primary 通道（驱动轴）
  /// @param buffer_1 secondary 通道（随从）
  AllLatest(const ChannelBuffer<M0>& buffer_0,
            const ChannelBuffer<M1>& buffer_1)
      : buffer_m0_(buffer_0),
        buffer_m1_(buffer_1),
        // fusion buffer 的容量 = primary 容量 - 1（与 CyberRT 一致）
        buffer_fusion_(
            buffer_m0_.channel_id(),
            std::make_shared<CacheBuffer<std::shared_ptr<FusionDataType>>>(
                buffer_0.Buffer()->Capacity() - 1)) {
    // 注册 DataNotifier 回调到 primary 通道。
    // DataDispatcher::Dispatch → DataNotifier::Notify(buffer_m0_.channel_id())
    // → 触发此回调 → 拉取两个通道的最新值 → 填充 fusion buffer。
    // 回调受 fusion buffer 的 mutex 保护，防止 DataFusion::Fusion()
    // 同时读 fusion buffer 导致 data race。
    notifier_ = std::make_shared<Notifier>();
    notifier_->callback = [this]() {
      std::shared_ptr<M0> m0;
      std::shared_ptr<M1> m1;
      // primary + secondary 都必须有数据。这里只 check Latest 的返回值：
      // primary 刚被 Fill 肯定有数据（刚 dispatch 的）；secondary 可能
      // 尚无数据则 silently return（fusion buffer 不变，Fusion() 返回 false）。
      if (!buffer_m0_.Latest(m0) || !buffer_m1_.Latest(m1)) {
        return;
      }

      auto data = std::make_shared<FusionDataType>(std::move(m0), std::move(m1));
      // Fill fusion buffer under its own mutex.
      {
        std::lock_guard<std::mutex> lg(buffer_fusion_.Buffer()->Mutex());
        buffer_fusion_.Buffer()->Fill(data);
      }
    };
    DataNotifier::Instance()->AddNotifier(buffer_m0_.channel_id(), notifier_);
  }

  /// 从 fusion buffer 取出一组融合数据。
  /// 语义与 ChannelBuffer::Fetch 一致：*index == 0 ⇒ 取最新；
  /// 否则取 *index 位置；消费者追平时返回 false。
  bool Fusion(uint64_t* index, std::shared_ptr<M0>& m0,
              std::shared_ptr<M1>& m1) override {
    std::shared_ptr<FusionDataType> fusion_data;
    if (!buffer_fusion_.Fetch(index, fusion_data)) {
      return false;
    }
    m0 = std::get<0>(*fusion_data);
    m1 = std::get<1>(*fusion_data);
    // 推进消费游标：ChannelBuffer::Fetch 将 *index 设为 Tail，
    // 调用方需 +1 使其指向"已消费的下一位置"，下次调用才能正确判断
    // "消费者已追平"（*index == Tail + 1）。
    ++(*index);
    return true;
  }

  /// 直接访问底层 fusion buffer 的 ChannelBuffer（供上层 DataVisitor 或
  /// DataNotifier 链式挂接使用）。
  const ChannelBuffer<FusionDataType>& fusion_buffer() const {
    return buffer_fusion_;
  }

 private:
  ChannelBuffer<M0> buffer_m0_;
  ChannelBuffer<M1> buffer_m1_;
  ChannelBuffer<FusionDataType> buffer_fusion_;
  std::shared_ptr<Notifier> notifier_;
};

}  // namespace fusion
}  // namespace data
}  // namespace minicyber

#endif  // MINICYBER_DATA_FUSION_ALL_LATEST_H_