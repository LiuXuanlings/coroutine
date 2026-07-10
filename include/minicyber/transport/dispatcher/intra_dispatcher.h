#ifndef MINICYBER_TRANSPORT_DISPATCHER_INTRA_DISPATCHER_H_
#define MINICYBER_TRANSPORT_DISPATCHER_INTRA_DISPATCHER_H_

#include <cstdint>
#include <memory>

#include "minicyber/data/data_dispatcher.h"

namespace minicyber {
namespace transport {

// =============================================================================
// IntraDispatcher：同进程内分发器
//
// 职责：把一条消息以"指针直接投递"的方式送进本进程的数据总线，
//       不经过任何序列化、不跨进程、不拷贝 payload。
//
// 设计：极薄包装。IntraDispatcher 本身不持有状态，所有路由工作交给
//       data::DataDispatcher<T> 单例完成：
//         Dispatch(channel_id, msg)
//           -> DataDispatcher<T>::Instance()->Dispatch(channel_id, msg)
//           -> 填充所有订阅该 channel 的 ChannelBuffer
//           -> DataNotifier::Notify(channel_id) 唤醒挂起在 DATA_WAIT 的协程
//
// 为什么仍要单独一个类（而不是让上层直接调 DataDispatcher）？
//   - 与 ShmDispatcher 提供统一接口，Step 23 的 Transport 可在运行时
//     根据 TopologyManager::IsSameProc 无感切换 INTRA / SHM 后端。
//   - 后续可在此挂载进程内独有逻辑（如 host_id 过滤、消息链）而不污染 DataDispatcher。
//
// 模板化：与 DataDispatcher<T> 一致，按消息类型实例化独立单例。
// =============================================================================

template <typename T>
class IntraDispatcher {
 public:
  static IntraDispatcher<T>* Instance() {
    static IntraDispatcher<T> inst;
    return &inst;
  }

  // 将消息以零拷贝方式投递到本进程数据总线。
  // 返回值与 DataDispatcher::Dispatch 一致：有订阅者则 true。
  bool Dispatch(uint64_t channel_id, const std::shared_ptr<T>& msg) {
    return data::DataDispatcher<T>::Instance()->Dispatch(channel_id, msg);
  }

 private:
  IntraDispatcher() = default;
  ~IntraDispatcher() = default;
  IntraDispatcher(const IntraDispatcher&) = delete;
  IntraDispatcher& operator=(const IntraDispatcher&) = delete;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_DISPATCHER_INTRA_DISPATCHER_H_