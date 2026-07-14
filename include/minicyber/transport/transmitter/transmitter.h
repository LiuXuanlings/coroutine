#ifndef MINICYBER_TRANSPORT_TRANSMITTER_TRANSMITTER_H_
#define MINICYBER_TRANSPORT_TRANSMITTER_TRANSMITTER_H_

#include <cstdint>
#include <memory>

namespace minicyber {
namespace transport {

// =============================================================================
// Transmitter：发布端抽象基类（对齐 CyberRT Transmitter<M>）
//
// 职责：定义统一的发布接口，具体后端（Intra / Shm）实现 Enable/Disable/Transmit。
//
// 与 CyberRT 的简化：
//   - 去掉 RoleAttributes / MessageInfo / Endpoint 基类，直接用 channel_id
//   - 去掉 PerfEventCache 性能埋点
//   - 保留 seq_num_：每条消息自增的序号，供后续按序消费使用
//
// 模板参数 M：消息类型。IntraTransmitter 按类型实例化；ShmTransmitter
// 固定为 std::string（见 shm_transmitter.h）。
// =============================================================================

template <typename M>
class Transmitter {
 public:
  using MessagePtr = std::shared_ptr<M>;

  explicit Transmitter(uint64_t channel_id) : channel_id_(channel_id) {}
  virtual ~Transmitter() = default;

  // 启用/禁用发布端。Enable 后 Transmit 才生效。
  virtual void Enable() = 0;
  virtual void Disable() = 0;

  // 发布一条消息。返回值由具体后端定义（通常：成功投递返回 true）。
  virtual bool Transmit(const MessagePtr& msg) = 0;

  bool enabled() const { return enabled_; }
  uint64_t channel_id() const { return channel_id_; }
  uint64_t seq_num() const { return seq_num_; }

 protected:
  // 子类在每次成功发布后调用，维护单调递增序号。
  uint64_t NextSeqNum() { return ++seq_num_; }

  uint64_t channel_id_;
  bool enabled_ = false;
  uint64_t seq_num_ = 0;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_TRANSMITTER_TRANSMITTER_H_