#ifndef MINICYBER_TRANSPORT_SHM_CONDITION_NOTIFIER_H_
#define MINICYBER_TRANSPORT_SHM_CONDITION_NOTIFIER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <sys/ipc.h>
#include <sys/types.h>

namespace minicyber {
namespace transport {

// =============================================================================
// ConditionNotifier（CyberRT 原生移植版：System V SHM + 轮询）
//
// 本文件是 eventfd+epoll 版本的同接口替代实现，用于性能对比。
// 主线（master）保留 eventfd+epoll 版本作为简历亮点；
// 本分支（feature/cyber-native-notifier）移植 CyberRT 原生方案，
// 在 Phase 5 benchmark 中作为基线进行对比。
//
// 机制：
//   - 使用 System V 共享内存（shmget/shmat）承载一个 Indicator 结构
//   - Indicator 是一个环形缓冲：next_seq 单调递增，infos/seqs 按 next_seq % kBufLength 索引
//   - Notify()：把 ReadableInfo 写入环形，原子累加 next_seq
//   - Listen()：轮询 next_seq 是否变化，变化则读出对应槽位的 info
//     —— 这是 CyberRT 的原生做法，50µs 粒度 sleep 规避忙等
//
// 与 eventfd 版本的架构差异：
//   - Fd() 返回 -1：本方案没有可注册进 epoll 的文件描述符
//   - 唤醒路径：必须由后台线程/协程主动调用 Listen() 轮询，
//     而非由 epoll_wait 优雅唤醒挂起协程
//   - 这是本分支与主线在简历叙事上的核心差异点
//
// ReadableInfo 简化：CyberRT 的 ReadableInfo 含 host_id/block_index/channel_id
// 并支持序列化；本移植保留三字段但简化为 POD 结构，不做序列化（跨进程
// 通过同一段 SHM 直接共享，无需序列化）。
// =============================================================================

constexpr uint32_t kBufLength = 4096;

struct ReadableInfo {
  uint64_t host_id = 0;
  uint32_t block_index = 0;
  uint64_t channel_id = 0;
};

class ConditionNotifier {
  static constexpr uint64_t kUnpublishedSeq = UINT64_MAX;

  struct Indicator {
    std::atomic<uint64_t> next_seq{0};
    std::atomic_flag writer_lock = ATOMIC_FLAG_INIT;
    ReadableInfo infos[kBufLength];
    std::atomic<uint64_t> seqs[kBufLength];

    Indicator() {
      for (auto& seq : seqs) {
        seq.store(kUnpublishedSeq, std::memory_order_relaxed);
      }
    }
  };

 public:
  ConditionNotifier();
  ~ConditionNotifier();

  ConditionNotifier(const ConditionNotifier&) = delete;
  ConditionNotifier& operator=(const ConditionNotifier&) = delete;

  // 初始化：创建或打开 System V 共享内存
  bool Init();

  // 发出一次通知：写一条 ReadableInfo 到环形，next_seq++
  bool Notify(const ReadableInfo& info);

  // 轮询等待通知：
  //   timeout_ms = -1 : 永久等待（循环 50µs 轮询）
  //   timeout_ms = 0  : 非阻塞轮询一次
  //   timeout_ms > 0  : 至多等待 timeout_ms 毫秒
  // 返回 true 表示收到通知，false 表示超时或已 shutdown
  bool Listen(int timeout_ms, ReadableInfo* info);

  // 本方案无可注册进 epoll 的 fd，始终返回 -1
  // 保留接口以便与 eventfd 版本同接口替换
  int Fd() const { return -1; }
  int EpollFd() const { return -1; }

  void Shutdown();
  bool IsShutdown() const { return shutdown_.load(); }

  // 测试辅助
  key_t key() const { return key_; }

 private:
  bool OpenOrCreate();
  bool OpenOnly();
  bool Remove();
  void Reset();
  bool BeginOperation();
  void EndOperation();

  key_t key_ = 0;
  void* managed_shm_ = nullptr;
  size_t shm_size_ = 0;
  Indicator* indicator_ = nullptr;
  uint64_t next_seq_ = 0;
  std::atomic<bool> shutdown_{false};
  std::mutex lifecycle_mutex_;
  std::condition_variable operations_finished_;
  size_t active_operations_ = 0;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_SHM_CONDITION_NOTIFIER_H_
