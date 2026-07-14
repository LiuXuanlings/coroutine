#ifndef MINICYBER_TRANSPORT_RECEIVER_RECEIVER_H_
#define MINICYBER_TRANSPORT_RECEIVER_RECEIVER_H_

#include <cstdint>
#include <functional>
#include <memory>

namespace minicyber {
namespace transport {

// =============================================================================
// Receiver：订阅端抽象基类（对齐 CyberRT Receiver<M>）
//
// 职责：定义统一的订阅接口，具体后端（Intra / Shm）实现 Enable/Disable，
//       数据到达时通过 OnNewMessage 回调上层。
//
// 与 CyberRT 的简化：
//   - 去掉 RoleAttributes / MessageInfo / Endpoint / History
//   - MessageListener 签名简化为 void(const shared_ptr<M>&)
//     （CyberRT 还带 MessageInfo + RoleAttributes，本移植不需要）
//
// 模板参数 M：消息类型。IntraReceiver 按类型实例化；ShmReceiver
// 固定为 std::string（见 shm_receiver.h）。
// =============================================================================

template <typename M>
class Receiver {
 public:
  using MessagePtr = std::shared_ptr<M>;
  using MessageListener = std::function<void(const MessagePtr&)>;

  Receiver(uint64_t channel_id, const MessageListener& msg_listener)
      : channel_id_(channel_id), msg_listener_(msg_listener) {}
  virtual ~Receiver() = default;

  // 启用/禁用订阅。Enable 后开始接收消息。
  virtual void Enable() = 0;
  virtual void Disable() = 0;

  bool enabled() const { return enabled_; }
  uint64_t channel_id() const { return channel_id_; }

 protected:// protected: 子类可访问
  // 子类在数据到达时调用，触发上层回调。
  void OnNewMessage(const MessagePtr& msg) {
    if (msg_listener_) msg_listener_(msg);
  }

  uint64_t channel_id_;
  MessageListener msg_listener_;
  bool enabled_ = false;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_RECEIVER_RECEIVER_H_