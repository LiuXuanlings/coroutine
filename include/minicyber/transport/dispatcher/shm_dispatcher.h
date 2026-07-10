#ifndef MINICYBER_TRANSPORT_DISPATCHER_SHM_DISPATCHER_H_
#define MINICYBER_TRANSPORT_DISPATCHER_SHM_DISPATCHER_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include "minicyber/data/data_dispatcher.h"
#include "minicyber/transport/shm/condition_notifier.h"
#include "minicyber/transport/shm/posix_segment.h"

namespace minicyber {
namespace transport {

// =============================================================================
// ShmDispatcher：跨进程共享内存分发器（CyberRT 原生基线对齐）
//
// 职责：后台线程轮询 ConditionNotifier，当对端进程写入 SHM 并 Notify 后，
//       本进程被唤醒，按 ReadableInfo 携带的 {channel_id, block_index}
//       直接读取对应 SHM 块，把 payload 拷成 std::string，注入本进程的
//       数据总线 DataDispatcher<std::string>，唤醒挂起在 DATA_WAIT 的协程。
//
// 路径（与 CyberRT 对齐）：
//   后台线程 ThreadFunc:
//     notifier_->Listen(100, &readable_info)
//       -> readable_info.channel_id / block_index
//       -> 查 segments_[channel_id] 拿到 PosixSegment
//       -> AcquireBlockToRead(block_index)
//       -> memcpy payload -> std::string
//       -> ReleaseReadBlock
//       -> DataDispatcher<std::string>::Instance()->Dispatch(channel_id, msg)
//
// 与 eventfd 版本的核心差异：
//   原生 ConditionNotifier 的 Listen 返回 ReadableInfo，直接给出目标 channel
//   和 block_index，无需扫描所有 segment。这是 SysV SHM + Indicator 方案的
//   优势：通知本身携带路由信息。
//
// 简化点（与 CyberRT 差异）：
//   1. 消息类型固定为 std::string（roadmap：先支持 string，预留二进制接口）
//   2. 不做 MessageInfo 序列化（CyberRT 在 payload 后跟一段 MessageInfo），
//      本移植只读 payload 本身，msg_size 由 Block::msg_size() 给出
//   3. host_id 过滤暂不启用（单机测试场景）
// =============================================================================

class ShmDispatcher {
 public:
  using SegmentContainer = std::unordered_map<uint64_t, std::shared_ptr<PosixSegment>>;

  static ShmDispatcher* Instance() {
    static ShmDispatcher inst;
    return &inst;
  }

  ~ShmDispatcher();

  // 为某 channel 注册一个 PosixSegment，开始监听该 channel 的跨进程消息
  void AddSegment(uint64_t channel_id);

  // 关闭后台线程与所有 segment
  void Shutdown();

  // 测试辅助：是否仍在运行
  bool IsRunning() const { return running_.load(); }

 private:
  ShmDispatcher();
  ShmDispatcher(const ShmDispatcher&) = delete;
  ShmDispatcher& operator=(const ShmDispatcher&) = delete;

  void Init();
  void ThreadFunc();
  void ReadMessage(uint64_t channel_id, uint32_t block_index);

  std::atomic<bool> running_{false};
  std::thread thread_;
  std::unique_ptr<ConditionNotifier> notifier_;
  SegmentContainer segments_;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_DISPATCHER_SHM_DISPATCHER_H_