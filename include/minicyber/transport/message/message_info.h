#ifndef MINICYBER_TRANSPORT_MESSAGE_MESSAGE_INFO_H_
#define MINICYBER_TRANSPORT_MESSAGE_MESSAGE_INFO_H_

#include <cstdint>
#include <string>

#include "minicyber/transport/common/identity.h"

// =============================================================================
// MessageInfo：传输层消息元数据（对齐 CyberRT transport::MessageInfo）
//
// 职责：携带消息的发送者身份、序号和信道标识。
//
// 字段说明：
//   sender_id_  : 发送端 Identity（区分消息来源端点）
//   channel_id_ : 信道标识（当前暂未使用，保留以对齐 CyberRT）
//   seq_num_    : 序列号
//   spare_id_   : 备用标识
//
// 与 CyberRT 的简化：
//   - 去掉 SerializeTo/DeserializeFrom（当前阶段不需要跨进程序列化）
//   - 去掉 kSize 静态常量（仅 RTPS 场景需要）
// =============================================================================

namespace minicyber {
namespace transport {

class MessageInfo {
 public:
  MessageInfo() = default;

  MessageInfo(const Identity& sender_id, uint64_t seq_num)
      : sender_id_(sender_id), seq_num_(seq_num) {}

  MessageInfo(const Identity& sender_id, uint64_t seq_num,
              const Identity& spare_id)
      : sender_id_(sender_id), seq_num_(seq_num), spare_id_(spare_id) {}

  MessageInfo(const MessageInfo& another)
      : sender_id_(another.sender_id_),
        channel_id_(another.channel_id_),
        seq_num_(another.seq_num_),
        spare_id_(another.spare_id_) {}

  virtual ~MessageInfo() = default;

  MessageInfo& operator=(const MessageInfo& another) {
    if (this != &another) {
      sender_id_ = another.sender_id_;
      channel_id_ = another.channel_id_;
      seq_num_ = another.seq_num_;
      spare_id_ = another.spare_id_;
    }
    return *this;
  }

  bool operator==(const MessageInfo& another) const {
    return sender_id_ == another.sender_id_ &&
           channel_id_ == another.channel_id_ &&
           seq_num_ == another.seq_num_ &&
           spare_id_ == another.spare_id_;
  }

  bool operator!=(const MessageInfo& another) const {
    return !(*this == another);
  }

  // Getters / Setters
  const Identity& sender_id() const { return sender_id_; }
  void set_sender_id(const Identity& sender_id) { sender_id_ = sender_id; }

  uint64_t channel_id() const { return channel_id_; }
  void set_channel_id(uint64_t channel_id) { channel_id_ = channel_id; }

  uint64_t seq_num() const { return seq_num_; }
  void set_seq_num(uint64_t seq_num) { seq_num_ = seq_num; }

  const Identity& spare_id() const { return spare_id_; }
  void set_spare_id(const Identity& spare_id) { spare_id_ = spare_id; }

 private:
  Identity sender_id_;
  uint64_t channel_id_ = 0;
  uint64_t seq_num_ = 0;
  Identity spare_id_;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_MESSAGE_MESSAGE_INFO_H_
