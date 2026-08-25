#ifndef MINICYBER_TRANSPORT_TRANSPORT_H_
#define MINICYBER_TRANSPORT_TRANSPORT_H_

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

#include <google/protobuf/message.h>

#include "minicyber/proto/role_attributes.pb.h"
#include "minicyber/transport/receiver/hybrid_receiver.h"
#include "minicyber/transport/receiver/receiver.h"
#include "minicyber/transport/transmitter/hybrid_transmitter.h"
#include "minicyber/transport/transmitter/transmitter.h"

namespace minicyber {
namespace transport {

// =============================================================================
// Transport：顶层路由工厂（对齐 CyberRT Transport 的子集）
//
// 职责：以完整 RoleAttributes 创建 Hybrid 端点，并持续接收 ChannelManager
// 的 Join/Leave 变化。同进程对端使用 INTRA，其他同机进程使用 SHM；两类
// 对端可以同时存在。公开工厂只接受 Protobuf 消息，旧固定 INTRA 泛型入口
// 已删除，避免绕过 Node 的发现生命周期和正式 Channel 类型边界。
//
// 与 CyberRT 的简化：
//   - 保留 RoleAttributes 与 Hybrid 的 per-opposite 路由职责
//   - 删除 RTPS 数据面和跨主机连接
//   - Transport 是纯静态工厂，无状态，不持有 dispatcher 引用
//
// channel 命名：业务层用 std::string channel（如 "/chatter"），
//   Transport 内部 hash 成 uint64_t channel_id 传给底层 Transmitter/Receiver。
// =============================================================================

class Transport {
 public:
  // 把 channel 名字 hash 成 uint64_t channel_id
  // 用简单 hash（与 ConditionNotifier 的 MakeKey 风格一致）
  static uint64_t ChannelNameToId(const std::string& channel) {
    uint64_t h = 0;
    for (char c : channel) {
      h = h * 131u + static_cast<uint64_t>(c);
    }
    return h;
  }

  // RoleAttributes 是发现层的唯一连接键。Hybrid 在
  // 创建后订阅 ChannelManager 变更，不依据端点创建瞬间的静态拓扑决定后端。
  template <typename T>
  static std::shared_ptr<HybridTransmitter<T>> CreateHybridTransmitter(
      const proto::RoleAttributes& attr) {
    static_assert(std::is_base_of<google::protobuf::Message, T>::value,
                  "HybridTransport messages must derive from google::protobuf::Message");
    auto tx = std::make_shared<HybridTransmitter<T>>(attr);
    tx->Enable();
    return tx;
  }

  template <typename T>
  static std::shared_ptr<HybridReceiver<T>> CreateHybridReceiver(
      const proto::RoleAttributes& attr,
      const typename Receiver<T>::MessageListener& msg_listener) {
    static_assert(std::is_base_of<google::protobuf::Message, T>::value,
                  "HybridTransport messages must derive from google::protobuf::Message");
    auto rx = std::make_shared<HybridReceiver<T>>(attr, msg_listener);
    rx->Enable();
    return rx;
  }
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_TRANSPORT_H_
