#ifndef MINICYBER_TRANSPORT_RECEIVER_SHM_RECEIVER_H_
#define MINICYBER_TRANSPORT_RECEIVER_SHM_RECEIVER_H_

#include <memory>
#include <string>

#include "minicyber/data/channel_buffer.h"
#include "minicyber/data/data_dispatcher.h"
#include "minicyber/data/data_notifier.h"
#include "minicyber/transport/dispatcher/shm_dispatcher.h"
#include "minicyber/transport/receiver/receiver.h"

namespace minicyber {
namespace transport {

// =============================================================================
// ShmReceiver：跨进程订阅端（对齐 CyberRT ShmReceiver）
//
// 职责：在 ShmDispatcher 的数据总线上订阅一个 channel，当对端进程
//       ShmTransmitter 写入 SHM 并 Notify 后，ShmDispatcher 后台线程
//       读取 SHM 块、拷成 std::string、注入 DataDispatcher<std::string>，
//       触发 DataNotifier 回调，本 receiver 从 ChannelBuffer 取出消息
//       回调上层。
//
// 数据路径（对齐 CyberRT ShmReceiver::Enable -> AddListener）：
//   对端: ShmTransmitter::Transmit
//     -> AcquireBlockToWrite -> memcpy -> ReleaseWrittenBlock
//     -> ConditionNotifier::Notify(ReadableInfo{0, block_index, channel_id})
//   本端: ShmDispatcher 后台线程
//     -> ConditionNotifier::Listen -> ReadableInfo
//     -> PosixSegment::AcquireBlockToRead -> memcpy -> std::string
//     -> DataDispatcher<std::string>::Dispatch -> 填充 ChannelBuffer
//     -> DataNotifier::Notify -> ShmReceiver 回调
//       -> ChannelBuffer::Latest -> OnNewMessage -> msg_listener_
//
// Enable():
//   1. ShmDispatcher::AddSegment(channel_id) —— 注册 PosixSegment，后台线程
//      开始监听该 channel 的跨进程通知
//   2. 创建 ChannelBuffer<std::string> 并注册到 DataDispatcher<std::string>
//   3. 注册 DataNotifier 回调：数据到达时 Latest 取消息，回调上层
// Disable():
//   重置回调 + 销毁 ChannelBuffer（weak_ptr 自动失效）
//
// 与 CyberRT 的简化：
//   - 消息类型固定为 std::string
//   - 不做 host_id 过滤（单机测试场景）
//   - ShmDispatcher 已是单例，无需持有引用
// =============================================================================

class ShmReceiver : public Receiver<std::string> {
 public:
  ShmReceiver(uint64_t channel_id, const MessageListener& msg_listener)
      : Receiver<std::string>(channel_id, msg_listener) {}
  ~ShmReceiver() override { Disable(); }

  void Enable() override {
    if (enabled_) return;

    // 1. 注册 PosixSegment 到 ShmDispatcher，后台线程开始监听该 channel
    ShmDispatcher::Instance()->AddSegment(channel_id_);

    // 2. 创建 ChannelBuffer 并注册到 DataDispatcher（订阅 channel）
    auto buffer =
        std::make_shared<data::ChannelBuffer<std::string>::BufferType>(10);
    cb_ = std::make_unique<data::ChannelBuffer<std::string>>(channel_id_, buffer);
    data::DataDispatcher<std::string>::Instance()->AddBuffer(*cb_);

    // 3. 注册 DataNotifier 回调：数据到达时从 ChannelBuffer 取最新消息回调上层
    notifier_ = std::make_shared<data::Notifier>();
    notifier_->callback = [this]() {
      if (!enabled_ || cb_ == nullptr) return;
      std::shared_ptr<std::string> msg;
      if (cb_->Latest(msg)) {
        OnNewMessage(msg);
      }
    };
    data::DataNotifier::Instance()->AddNotifier(channel_id_, notifier_);

    enabled_ = true;
  }

  void Disable() override {
    if (!enabled_) return;
    enabled_ = false;
    if (notifier_) {
      notifier_->callback = nullptr;
      notifier_.reset();
    }
    cb_.reset();
  }

 private:
  std::unique_ptr<data::ChannelBuffer<std::string>> cb_;
  std::shared_ptr<data::Notifier> notifier_;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_RECEIVER_SHM_RECEIVER_H_