#ifndef MINICYBER_TRANSPORT_RECEIVER_SHM_RECEIVER_H_
#define MINICYBER_TRANSPORT_RECEIVER_SHM_RECEIVER_H_

#include <memory>
#include <mutex>
#include <string>
#include <type_traits>

#include <google/protobuf/message.h>

#include "minicyber/data/data_dispatcher.h"
#include "minicyber/transport/dispatcher/shm_dispatcher.h"
#include "minicyber/transport/receiver/receiver.h"

namespace minicyber {
namespace transport {

// SHM 接收端仅从共享块恢复 Protobuf。恢复后的对象先进入 DataDispatcher，
// 再通知 Reader，保持 cyber_ref 的 DATA_WAIT 唤醒链而不让发布线程同步执行 Proc。
template <typename M>
class ShmReceiver : public Receiver<M> {
  static_assert(std::is_base_of<google::protobuf::Message, M>::value,
                "ShmTransport messages must derive from google::protobuf::Message");

 public:
  using Base = Receiver<M>;
  using MessageListener = typename Base::MessageListener;

  ShmReceiver(uint64_t channel_id, const MessageListener& msg_listener)
      : Base(channel_id, msg_listener) {}
  ~ShmReceiver() override { Disable(); }

  void Enable() override {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (listener_id_ != 0) return;
    listener_id_ = ShmDispatcher::Instance()->AddListener(
        this->channel_id_, [this](const std::string& payload) {
          auto msg = std::make_shared<M>();
          if (!msg->ParseFromString(payload)) return;
          data::DataDispatcher<M>::Instance()->DispatchFromShm(
              this->channel_id_, msg);
          this->OnNewMessage(msg);
        });
    this->enabled_ = listener_id_ != 0;
  }

  void Disable() override {
    uint64_t listener_id = 0;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      listener_id = listener_id_;
      listener_id_ = 0;
      this->enabled_ = false;
    }
    if (listener_id != 0) {
      ShmDispatcher::Instance()->RemoveListener(this->channel_id_, listener_id);
    }
  }

 private:
  std::mutex lifecycle_mutex_;
  uint64_t listener_id_ = 0;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_RECEIVER_SHM_RECEIVER_H_
