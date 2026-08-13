#ifndef MINICYBER_TRANSPORT_TRANSPORT_H_
#define MINICYBER_TRANSPORT_TRANSPORT_H_

#include <cstdint>
#include <memory>
#include <string>
#include "minicyber/transport/receiver/intra_receiver.h"
#include "minicyber/transport/receiver/receiver.h"
#include "minicyber/transport/transmitter/intra_transmitter.h"
#include "minicyber/transport/transmitter/transmitter.h"

namespace minicyber {
namespace transport {

// =============================================================================
// Transport：顶层路由工厂（对齐 CyberRT Transport 的子集）
//
// 职责：为本地 Node 自动创建 INTRA（同进程零拷贝）后端。
// 跨进程 SHM 由调用方显式创建 ShmTransmitter/ShmReceiver。
//
// MiniCyber 没有原生 HybridTransport 所需的动态拓扑发现及端点变更通知。
// 因此工厂不能在端点创建时依据一份可能不完整的静态拓扑选择后端：
// Writer 先创建为 SHM、随后 Reader 创建为 INTRA 会使两端落在不同总线上。
// 自动工厂固定选择 INTRA，避免创建顺序影响本地通信语义。
//
// 与 CyberRT 的简化：
//   - 去掉 RoleAttributes / OptionalMode / QosProfile / Participant
//   - 去掉 HYBRID 和 RTPS 路由（CyberRT 依动态拓扑切换）
//   - Transport 是纯静态工厂，无状态，不持有 dispatcher 引用（已是单例）
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

  // 创建本地发布端。
  template <typename T>
  static std::shared_ptr<Transmitter<T>> CreateTransmitter(
      const std::string& channel) {
    uint64_t channel_id = ChannelNameToId(channel);
    auto tx = std::make_shared<IntraTransmitter<T>>(channel_id);

    if (tx) tx->Enable();
    return tx;
  }

  // 创建本地订阅端。
  template <typename T>
  static std::shared_ptr<Receiver<T>> CreateReceiver(
      const std::string& channel,
      const typename Receiver<T>::MessageListener& msg_listener) {
    uint64_t channel_id = ChannelNameToId(channel);
    auto rx = std::make_shared<IntraReceiver<T>>(channel_id, msg_listener);

    if (rx) rx->Enable();
    return rx;
  }
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_TRANSPORT_H_
