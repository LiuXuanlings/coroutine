#ifndef MINICYBER_TRANSPORT_TRANSMITTER_INTRA_TRANSMITTER_H_
#define MINICYBER_TRANSPORT_TRANSMITTER_INTRA_TRANSMITTER_H_

#include <memory>
#include <mutex>

#include "minicyber/transport/dispatcher/intra_dispatcher.h"
#include "minicyber/transport/transmitter/transmitter.h"

namespace minicyber {
namespace transport {

// =============================================================================
// IntraTransmitter：同进程内发布端（零拷贝）
//
// 职责：把 shared_ptr<M> 以指针直接投递的方式送进本进程数据总线，
//       不经过任何序列化、不跨进程、不拷贝 payload。
//
// 路径（对齐 CyberRT IntraTransmitter::Transmit）：
//   Transmit(msg)
//     -> IntraDispatcher<M>::Instance()->Dispatch(channel_id_, msg)
//     -> DataDispatcher<M>::Dispatch -> 填充所有订阅 ChannelBuffer
//     -> DataNotifier::Notify -> 唤醒挂起在 DATA_WAIT 的协程
//
// 模板参数 M：与 IntraDispatcher / DataDispatcher 一致。
// =============================================================================

template <typename M>
class IntraTransmitter : public Transmitter<M> {
 public:
  explicit IntraTransmitter(uint64_t channel_id)
      : Transmitter<M>(channel_id) {}
  ~IntraTransmitter() override { Disable(); }

  void Enable() override {
    std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
    if (this->enabled_) return;
    // IntraDispatcher 是单例，无需持有；标记启用即可。
    this->enabled_ = true;
  }

  void Disable() override {
    std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
    this->enabled_ = false;
  }

  bool Transmit(const std::shared_ptr<M>& msg) override {
    std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
    if (!this->enabled_) return false;
    // 与 CyberRT IntraTransmitter 一致：Dispatch 把消息投递进数据总线
    // （填充所有 ChannelBuffer + 触发 DataNotifier），无论是否有订阅者都
    // 视为发布成功。返回值来自 DataDispatcher::Dispatch，但语义上发布即成功。
    IntraDispatcher<M>::Instance()->Dispatch(this->channel_id_, msg);
    this->NextSeqNum();
    return true;
  }

 private:
  std::recursive_mutex lifecycle_mutex_;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_TRANSMITTER_INTRA_TRANSMITTER_H_
