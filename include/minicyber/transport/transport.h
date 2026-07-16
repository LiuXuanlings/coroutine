#ifndef MINICYBER_TRANSPORT_TRANSPORT_H_
#define MINICYBER_TRANSPORT_TRANSPORT_H_

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

#include "minicyber/topology/topology_manager.h"
#include "minicyber/transport/receiver/intra_receiver.h"
#include "minicyber/transport/receiver/receiver.h"
#include "minicyber/transport/receiver/shm_receiver.h"
#include "minicyber/transport/transmitter/intra_transmitter.h"
#include "minicyber/transport/transmitter/shm_transmitter.h"
#include "minicyber/transport/transmitter/transmitter.h"

namespace minicyber {
namespace transport {

// =============================================================================
// Transport：顶层路由工厂（对齐 CyberRT Transport 的子集）
//
// 职责：根据 TopologyManager 的拓扑关系，自动为每个 channel 选择
//   INTRA（同进程零拷贝）或 SHM（跨进程共享内存）后端，对上层完全透明。
//
// 路由决策：
//   UseShm(channel) = !TopologyManager::IsSameProc(channel)
//   - IsSameProc true  -> INTRA（IntraTransmitter / IntraReceiver）
//   - IsSameProc false -> SHM  （ShmTransmitter  / ShmReceiver）
//
// 类型约束：
//   - SHM 后端当前仅支持 std::string（memcpy + Block::msg_size）
//   - 对于非 std::string 类型，编译期 if constexpr 强制走 INTRA
//   - roadmap：未来扩展二进制序列化后可支持任意类型走 SHM
//
// 与 CyberRT 的简化：
//   - 去掉 RoleAttributes / OptionalMode / QosProfile / Participant
//   - 去掉 HYBRID 模式（CyberRT 用 HybridTransmitter 动态切换）
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

  // 是否走 SHM：拓扑未注册或跨进程时返回 true
  static bool UseShm(const std::string& channel) {
    return !topology::TopologyManager::Instance()->IsSameProc(channel);
  }

  // 创建发布端：根据拓扑自动选 Intra 或 Shm
  template <typename T>
  static std::shared_ptr<Transmitter<T>> CreateTransmitter(
      const std::string& channel) {
    uint64_t channel_id = ChannelNameToId(channel);
    bool use_shm = UseShm(channel);

    std::shared_ptr<Transmitter<T>> tx;

    if constexpr (std::is_same_v<T, std::string>) {
      // string 类型：可在 INTRA 与 SHM 间选择
      if (use_shm) {
        auto shm_tx = std::make_shared<ShmTransmitter>(channel_id);
        tx = shm_tx;  // ShmTransmitter 继承 Transmitter<std::string>
      } else {
        tx = std::make_shared<IntraTransmitter<T>>(channel_id);
      }
    } else {
      // 非 string：SHM 暂不支持，强制走 INTRA
      tx = std::make_shared<IntraTransmitter<T>>(channel_id);
    }

    if (tx) tx->Enable();
    return tx;
  }

  // 创建订阅端：根据拓扑自动选 Intra 或 Shm
  template <typename T>
  static std::shared_ptr<Receiver<T>> CreateReceiver(
      const std::string& channel,
      const typename Receiver<T>::MessageListener& msg_listener) {
    uint64_t channel_id = ChannelNameToId(channel);
    bool use_shm = UseShm(channel);

    std::shared_ptr<Receiver<T>> rx;

    if constexpr (std::is_same_v<T, std::string>) {
      if (use_shm) {
        rx = std::make_shared<ShmReceiver>(channel_id, msg_listener);
      } else {
        rx = std::make_shared<IntraReceiver<T>>(channel_id, msg_listener);
      }
    } else {
      rx = std::make_shared<IntraReceiver<T>>(channel_id, msg_listener);
    }

    if (rx) rx->Enable();
    return rx;
  }
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_TRANSPORT_H_