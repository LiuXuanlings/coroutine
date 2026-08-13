#ifndef MINICYBER_TRANSPORT_RECEIVER_INTRA_RECEIVER_H_
#define MINICYBER_TRANSPORT_RECEIVER_INTRA_RECEIVER_H_

#include <memory>

#include "minicyber/data/channel_buffer.h"
#include "minicyber/data/data_dispatcher.h"
#include "minicyber/data/data_notifier.h"
#include "minicyber/transport/receiver/receiver.h"

namespace minicyber {
namespace transport {

// =============================================================================
// IntraReceiver：同进程内订阅端
//
// 职责：在本进程的数据总线上订阅一个 channel，数据到达时通过
//       MessageListener 回调上层。
//
// 机制（对齐 CyberRT IntraReceiver::Enable -> AddListener）：
//   Enable():
//     1. 创建 ChannelBuffer<T> 并注册到 DataDispatcher<T>（订阅 channel）
//     2. 注册 DataNotifier 回调：数据到达时从 ChannelBuffer Fetch 出消息，
//        调用 OnNewMessage -> msg_listener_
//   Disable():
//     重置 Notifier 回调（ChannelBuffer 变为 DataDispatcher 中的 dead weak_ptr，
//     下次 Dispatch 自动跳过，无需显式注销）
//
// 为什么用 DataNotifier 回调而非直接在 Dispatch 中调用？
//   DataDispatcher::Dispatch 填充 ChannelBuffer 后调用 DataNotifier::Notify，
//   Notify 触发所有注册回调。IntraReceiver 借此挂接到"数据到达"事件，
//   从 ChannelBuffer 取出消息投递给上层。这与 ShmReceiver 的路径一致
//   （ShmDispatcher 后台线程 Dispatch -> DataNotifier -> ShmReceiver 回调）。

// 发送线程调用 Dispatch
//   → DataDispatcher 把消息写入对应 channel 的 ChannelBuffer（只负责存数据）
//   → DataDispatcher 调用 DataNotifier::Notify(channel_id)（只发一个“有新数据了”的信号）
//     → DataNotifier 遍历该 channel 下所有注册的通知回调
//       → 触发 IntraReceiver 注册的 lambda 回调
//         → IntraReceiver 自己从 ChannelBuffer 里取出最新消息
//           → 调用上层业务的 MessageListener
//
// 模板参数 M：与 DataDispatcher / ChannelBuffer 一致。
// =============================================================================

template <typename M>
class IntraReceiver : public Receiver<M> {
 public:
  using Base = Receiver<M>;
  //typename hear is necessary because MessagePtr is a dependent type,otherwise compiler will treat it as a non-type name
  using MessagePtr = typename Base::MessagePtr;
  using MessageListener = typename Base::MessageListener;

  IntraReceiver(uint64_t channel_id, const MessageListener& msg_listener)
      : Base(channel_id, msg_listener) {}
  ~IntraReceiver() override { Disable(); }

  void Enable() override {
    if (this->enabled_) return;

    // 创建 ChannelBuffer 并注册到 DataDispatcher（订阅 channel）
    auto buffer =
        std::make_shared<typename data::ChannelBuffer<M>::BufferType>(10);
    cb_ = std::make_unique<data::ChannelBuffer<M>>(this->channel_id_, buffer);
    data::DataDispatcher<M>::Instance()->AddBuffer(*cb_);

    // 注册 DataNotifier 回调：数据到达时从 ChannelBuffer 取最新消息回调上层。
    // 每次 Dispatch 填入一条消息并 Notify 一次，Latest() 取到的即本次到达的消息。
    notifier_ = std::make_shared<data::Notifier>();
    notifier_->SetCallback([this]() {
      if (!this->enabled_ || cb_ == nullptr) return;
      std::shared_ptr<M> msg;
      if (cb_->Latest(msg)) {
        this->OnNewMessage(msg);
      }
    });
    data::DataNotifier::Instance()->AddNotifier(this->channel_id_, notifier_);

    this->enabled_ = true;
  }

  void Disable() override {
    if (!this->enabled_) return;
    this->enabled_ = false;
    data::DataNotifier::Instance()->RemoveNotifier(this->channel_id_, notifier_);
    data::DataDispatcher<M>::Instance()->RemoveBuffer(*cb_);
    if (notifier_) {
      notifier_.reset();
    }
    cb_.reset();
  }

 private:
  std::unique_ptr<data::ChannelBuffer<M>> cb_;
  std::shared_ptr<data::Notifier> notifier_;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_RECEIVER_INTRA_RECEIVER_H_
