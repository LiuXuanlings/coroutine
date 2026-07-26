#ifndef MINICYBER_TRANSPORT_COMMON_IDENTITY_H_
#define MINICYBER_TRANSPORT_COMMON_IDENTITY_H_

#include <cstdint>
#include <cstring>
#include <random>
#include <string>

// =============================================================================
// Identity：传输层身份标识（对齐 CyberRT transport::Identity）
//
// 职责：为每个 Transport 端点（Transmitter/Receiver/Service/Client）生成
//   唯一的 8 字节标识符，用于消息路由、sender_id 匹配和请求追踪。
//
// 简化 vs CyberRT：
//   - CyberRT 底层使用 Poco UUID（36字节字符串 + hash），这里用 8 字节随机数
//   - 去掉 SequenceNumber（用的频率很低，Step 33 在 MessageInfo 中用 uint64_t）
//   - 去掉 Deserialize/Serialize（数据量小，memcpy 即可）
//
// 面试口径（Identity 的设计动机）：
//   "Identity 是传输层的端点指纹。在 RPC 场景中，Client 需要确保收到的
//   Response 匹配自己发出的 Request。Identity 提供了轻量的发送者身份标记，
//   配合 seq_num 可以实现 O(1) 的 pending_requests 查找。8 字节的碰撞概率
//   在单进程内可忽略（2^64 空间）。"
// =============================================================================

namespace minicyber {
namespace transport {

constexpr uint8_t ID_SIZE = 8;

class Identity {
 public:
  /// 生成随机标识（默认行为）
  Identity() { Generate(); }

  /// 显式控制是否生成随机值
  explicit Identity(bool need_generate) {
    if (need_generate) Generate();
  }

  Identity(const Identity& another) { std::memcpy(data_, another.data_, ID_SIZE); hash_value_ = another.hash_value_; }

  virtual ~Identity() = default;

  Identity& operator=(const Identity& another) {
    if (this != &another) {
      std::memcpy(data_, another.data_, ID_SIZE);
      hash_value_ = another.hash_value_;
    }
    return *this;
  }

  bool operator==(const Identity& another) const {
    return std::memcmp(data_, another.data_, ID_SIZE) == 0;
  }

  bool operator!=(const Identity& another) const { return !(*this == another); }

  /// 返回标识符的十六进制字符串表示
  std::string ToString() const {
    static const char hex[] = "0123456789ABCDEF";
    std::string s(ID_SIZE * 2, ' ');
    for (size_t i = 0; i < ID_SIZE; ++i) {
      s[i * 2] = hex[(data_[i] >> 4) & 0x0F];
      s[i * 2 + 1] = hex[data_[i] & 0x0F];
    }
    return s;
  }

  size_t Length() const { return ID_SIZE; }

  uint64_t HashValue() const { return hash_value_; }

  const char* data() const { return data_; }
  void set_data(const char* data) {
    if (data == nullptr) return;
    std::memcpy(data_, data, ID_SIZE);
    UpdateHash();
  }

 private:
  void Generate() {
    std::random_device rd;
    for (size_t i = 0; i < ID_SIZE; ++i) {
      data_[i] = static_cast<char>(rd());
    }
    UpdateHash();
  }

  void UpdateHash() {
    hash_value_ = 0;
    for (size_t i = 0; i < ID_SIZE; ++i) {
      hash_value_ = hash_value_ * 131 + static_cast<uint64_t>(data_[i]);
    }
  }

  char data_[ID_SIZE]{};
  uint64_t hash_value_ = 0;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_COMMON_IDENTITY_H_