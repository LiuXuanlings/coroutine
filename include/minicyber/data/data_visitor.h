#ifndef MINICYBER_DATA_DATA_VISITOR_H_
#define MINICYBER_DATA_DATA_VISITOR_H_

#include <memory>

#include "minicyber/common/types.h"
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

// DataVisitor 只保存 ChannelBuffer、消费游标和数据到达 notifier。它不执行
// Component 业务回调；RoutineFactory 在 CRoutine 中调用 TryFetch 和 Proc，
// 对齐 CyberRT data::DataVisitor 与 croutine::RoutineFactory 的职责边界。
// 终局主干只需要单通道和首通道驱动的双通道 AllLatest，三/四输入不再提供。
template <typename M0, typename M1 = NullType>
class DataVisitor;

template <typename M0>
class DataVisitor<M0, NullType> : public DataVisitorBase {
 public:
  using BufferType = CacheBuffer<std::shared_ptr<M0>>;

  explicit DataVisitor(const VisitorConfig& config)
      : buffer_(config.channel_id,
                std::make_shared<BufferType>(config.queue_size)) {
    DataDispatcher<M0>::Instance()->AddBuffer(buffer_);
    data_notifier_->AddNotifier(buffer_.channel_id(), notifier_);
  }

  ~DataVisitor() {
    // 先从 notifier 表移除并等待在途回调，再移除弱引用 Buffer。这样关闭后
    // 不会再从发布线程进入已销毁的唤醒回调。
    data_notifier_->RemoveNotifier(buffer_.channel_id(), notifier_);
    DataDispatcher<M0>::Instance()->RemoveBuffer(buffer_);
  }

  bool TryFetch(std::shared_ptr<M0>& message) {
    if (!buffer_.Fetch(&next_msg_index_, message)) {
      return false;
    }
    ++next_msg_index_;
    return true;
  }

  uint64_t channel_id() const { return buffer_.channel_id(); }
  const ChannelBuffer<M0>& buffer() const { return buffer_; }

 private:
  ChannelBuffer<M0> buffer_;
};

template <typename M0, typename M1>
class DataVisitor : public DataVisitorBase {
 public:
  using PrimaryBufferType = CacheBuffer<std::shared_ptr<M0>>;
  using SecondaryBufferType = CacheBuffer<std::shared_ptr<M1>>;

  DataVisitor(const VisitorConfig& primary_config,
              const VisitorConfig& secondary_config)
      : primary_(primary_config.channel_id,
                 std::make_shared<PrimaryBufferType>(primary_config.queue_size)),
        secondary_(secondary_config.channel_id,
                   std::make_shared<SecondaryBufferType>(secondary_config.queue_size)),
        fusion_(std::make_unique<fusion::AllLatest<M0, M1>>(primary_, secondary_)) {
    DataDispatcher<M0>::Instance()->AddBuffer(primary_);
    DataDispatcher<M1>::Instance()->AddBuffer(secondary_);
    // AllLatest 已在 primary_ 的 Fill 回调中安装融合；DataDispatcher 在 Fill
    // 后才 Notify，因此这里的唤醒回调总在融合队列填充之后执行。
    data_notifier_->AddNotifier(primary_.channel_id(), notifier_);
  }

  ~DataVisitor() {
    data_notifier_->RemoveNotifier(primary_.channel_id(), notifier_);
    fusion_.reset();
    DataDispatcher<M0>::Instance()->RemoveBuffer(primary_);
    DataDispatcher<M1>::Instance()->RemoveBuffer(secondary_);
  }

  bool TryFetch(std::shared_ptr<M0>& primary_message,
                std::shared_ptr<M1>& secondary_message) {
    if (!fusion_->Fusion(&next_msg_index_, primary_message, secondary_message)) {
      return false;
    }
    ++next_msg_index_;
    return true;
  }

  uint64_t channel_id() const { return primary_.channel_id(); }
  uint64_t primary_channel_id() const { return primary_.channel_id(); }
  uint64_t secondary_channel_id() const { return secondary_.channel_id(); }
  const ChannelBuffer<M0>& primary_buffer() const { return primary_; }
  const ChannelBuffer<M1>& secondary_buffer() const { return secondary_; }

 private:
  ChannelBuffer<M0> primary_;
  ChannelBuffer<M1> secondary_;
  std::unique_ptr<fusion::AllLatest<M0, M1>> fusion_;
};

}  // namespace data
}  // namespace minicyber

#endif  // MINICYBER_DATA_DATA_VISITOR_H_
