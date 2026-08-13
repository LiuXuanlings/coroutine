#ifndef MINICYBER_TRANSPORT_RECEIVER_INTRA_RECEIVER_H_
#define MINICYBER_TRANSPORT_RECEIVER_INTRA_RECEIVER_H_

#include <atomic>
#include <memory>
#include <mutex>

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
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (subscription_) return;

    // 创建 ChannelBuffer 并注册到 DataDispatcher（订阅 channel）
    auto buffer =
        std::make_shared<typename data::ChannelBuffer<M>::BufferType>(10);
    auto subscription = std::make_shared<Subscription>(this->channel_id_, buffer);
    data::DataDispatcher<M>::Instance()->AddBuffer(subscription->channel_buffer);

    // 注册 DataNotifier 回调：数据到达时从 ChannelBuffer 取最新消息回调上层。
    // 每次 Dispatch 填入一条消息并 Notify 一次，Latest() 取到的即本次到达的消息。
    std::weak_ptr<Subscription> weak_subscription(subscription);
    subscription->notifier->SetCallback([this, weak_subscription]() {
      auto subscription = weak_subscription.lock();
      if (!subscription) return;
      if (!subscription->active.load(std::memory_order_acquire)) return;
      std::shared_ptr<M> msg;
      if (subscription->channel_buffer.Latest(msg) &&
          subscription->active.load(std::memory_order_acquire)) {
        this->OnNewMessage(msg);
      }
    });
    data::DataNotifier::Instance()->AddNotifier(this->channel_id_,
                                                subscription->notifier);

    subscription_ = std::move(subscription);
    this->enabled_ = true;
    subscription_->active.store(true, std::memory_order_release);
  }

  void Disable() override {
    std::shared_ptr<Subscription> subscription;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      if (!subscription_) return;
      subscription = std::move(subscription_);
      subscription->active.store(false, std::memory_order_release);
      this->enabled_ = false;
    }

    // RemoveNotifier waits for callbacks started by other threads. Do not hold
    // lifecycle_mutex_ here: a listener may itself call Disable().
    data::DataNotifier::Instance()->RemoveNotifier(this->channel_id_,
                                                    subscription->notifier);
    data::DataDispatcher<M>::Instance()->RemoveBuffer(subscription->channel_buffer);
  }

 private:
  struct Subscription {
    Subscription(uint64_t channel_id,
                 const std::shared_ptr<typename data::ChannelBuffer<M>::BufferType>& buffer)
        : channel_buffer(channel_id, buffer),
          notifier(std::make_shared<data::Notifier>()) {}

    data::ChannelBuffer<M> channel_buffer;
    std::shared_ptr<data::Notifier> notifier;
    std::atomic<bool> active{false};
  };

  std::mutex lifecycle_mutex_;
  std::shared_ptr<Subscription> subscription_;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_RECEIVER_INTRA_RECEIVER_H_
