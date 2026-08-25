#ifndef MINICYBER_DATA_FUSION_ALL_LATEST_H_
#define MINICYBER_DATA_FUSION_ALL_LATEST_H_

#include <memory>
#include <mutex>
#include <tuple>

#include "minicyber/data/channel_buffer.h"
#include "minicyber/data/fusion/data_fusion.h"

namespace minicyber {
namespace data {
namespace fusion {

// AllLatest 对齐 CyberRT 的双通道主通道驱动语义：主通道 Fill 时取副通道
// 的最新值并写入独立融合队列。副通道到达不会自行产生融合结果，因此它不是
// 时间戳严格对齐或 Barrier；融合队列所有权由本对象独占。
template <typename M0, typename M1>
class AllLatest : public DataFusion<M0, M1> {
  using FusionData = std::tuple<std::shared_ptr<M0>, std::shared_ptr<M1>>;

 public:
  AllLatest(const ChannelBuffer<M0>& primary,
            const ChannelBuffer<M1>& secondary)
      : primary_(primary),
        secondary_(secondary),
        fusion_buffer_(primary.channel_id(),
                       std::make_shared<CacheBuffer<std::shared_ptr<FusionData>>>(
                           primary.Buffer()->Capacity() - 1)) {
    // DataDispatcher 先 Fill，再 Notify。因而此回调在 Visitor 的 notifier
    // 唤醒协程前完成融合填充，协程被唤醒后不会观察到空的融合队列。
    primary_.Buffer()->SetFusionCallback(
        [this](const std::shared_ptr<M0>& primary_message) {
          std::shared_ptr<M1> secondary_message;
          if (!secondary_.Latest(secondary_message)) {
            return;
          }
          std::lock_guard<std::mutex> lock(fusion_buffer_.Buffer()->Mutex());
          fusion_buffer_.Buffer()->Fill(
              std::make_shared<FusionData>(primary_message, secondary_message));
        });
  }

  ~AllLatest() override {
    // SetFusionCallback waits for an in-flight Fill callback before replacing
    // it, so the callback cannot retain this after fusion state is destroyed.
    primary_.Buffer()->SetFusionCallback({});
  }

  bool Fusion(uint64_t* index, std::shared_ptr<M0>& primary_message,
              std::shared_ptr<M1>& secondary_message) override {
    std::shared_ptr<FusionData> data;
    if (!fusion_buffer_.Fetch(index, data)) {
      return false;
    }
    primary_message = std::get<0>(*data);
    secondary_message = std::get<1>(*data);
    return true;
  }

 private:
  ChannelBuffer<M0> primary_;
  ChannelBuffer<M1> secondary_;
  ChannelBuffer<FusionData> fusion_buffer_;
};

}  // namespace fusion
}  // namespace data
}  // namespace minicyber

#endif  // MINICYBER_DATA_FUSION_ALL_LATEST_H_
