#ifndef MINICYBER_TRANSPORT_DISPATCHER_SHM_DISPATCHER_H_
#define MINICYBER_TRANSPORT_DISPATCHER_SHM_DISPATCHER_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <functional>
#include <condition_variable>

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
//       直接读取对应 SHM 块，把 payload 拷成进程内字节副本；兼容路径注入
//       DataDispatcher<std::string>，Protobuf 路径交给带注销语义的原始字节监听器。
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
//   1. SHM 块只保存字节；字符串路径仅用于历史底层测试，Hybrid 用 Protobuf 字节监听器
//   2. 不做 MessageInfo 序列化（CyberRT 在 payload 后跟一段 MessageInfo），
//      本移植只读 payload 本身，msg_size 由 Block::msg_size() 给出
//   3. host_id 过滤暂不启用（单机测试场景）
// =============================================================================

class ShmDispatcher {
 public:
  using SegmentContainer = std::unordered_map<uint64_t, std::shared_ptr<PosixSegment>>;
  using RawMessageListener = std::function<void(const std::string&)>;

  static ShmDispatcher* Instance() {
    static ShmDispatcher inst;
    return &inst;
  }

  ~ShmDispatcher();

  // 为某 channel 注册一个 PosixSegment，开始监听该 channel 的跨进程消息
  bool AddSegment(uint64_t channel_id, uint64_t ceiling_msg_size = 1024,
                  uint32_t block_num = 4);

  // 注册 Protobuf 字节监听器。数据仍由 dispatcher 统一取得并释放 SHM 块，
  // 监听器只持有本次消息的进程内副本；RemoveListener 返回后不会再进入回调。
  uint64_t AddListener(uint64_t channel_id, RawMessageListener callback,
                       uint64_t ceiling_msg_size =
                           kDefaultShmCeilingMessageSize,
                       uint32_t block_num = kDefaultShmBlockNum);
  void RemoveListener(uint64_t channel_id, uint64_t listener_id);

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
  void NotifyRawListeners(uint64_t channel_id, const std::string& payload);

  struct RawListener {
    explicit RawListener(RawMessageListener listener)
        : callback(std::move(listener)) {}
    RawMessageListener callback;
    std::mutex mutex;
    std::condition_variable idle;
    bool active = true;
    size_t in_flight = 0;
  };

  std::atomic<bool> running_{false};
  std::thread thread_;
  std::unique_ptr<ConditionNotifier> notifier_;
  std::mutex segments_mutex_;
  SegmentContainer segments_;
  std::mutex listeners_mutex_;
  std::unordered_map<uint64_t,
                     std::unordered_map<uint64_t, std::shared_ptr<RawListener>>>
      listeners_;
  std::atomic<uint64_t> next_listener_id_{1};
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_DISPATCHER_SHM_DISPATCHER_H_
