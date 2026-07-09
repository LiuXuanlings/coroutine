#ifndef MINICYBER_TRANSPORT_SHM_BLOCK_H_
#define MINICYBER_TRANSPORT_SHM_BLOCK_H_

#include <atomic>
#include <cstdint>

namespace minicyber {
namespace transport {

class Segment;

// =============================================================================
// SHM 消息块 (Block)
//
// 每个 Block 是共享内存中一条消息的载体，由两部分组成：
//   1. 控制元数据（本类）：msg_size_ / msg_info_size_ / lock_num_
//   2. 实际 payload（紧随其后的连续字节，由 Segment 负责偏移计算）
//
// 锁语义（与 CyberRT 一致，刻意不复用 AtomicRWLock）：
//   - lock_num_ == 0 (kRWLockFree)        : 空闲
//   - lock_num_ == -1 (kWriteExclusive)   : 写独占
//   - lock_num_ > 0                        : 当前读者数
//
// 为什么不用 base/AtomicRWLock？
//   Block 驻留在共享内存里，需跨进程可见。AtomicRWLock 含
// write_lock_wait_num_、CACHELINE 对齐等进程内字段，跨进程语义不清晰。
// 这里保持 Block 极简：单个 atomic<int32_t> 即可表达读写锁状态。
// =============================================================================

class Block {
  friend class Segment;
  friend class ShmBlockTest;

 public:
  Block() : msg_size_(0), msg_info_size_(0) {}
  ~Block() = default;

  uint64_t msg_size() const { return msg_size_; }
  void set_msg_size(uint64_t msg_size) { msg_size_ = msg_size; }

  uint64_t msg_info_size() const { return msg_info_size_; }
  void set_msg_info_size(uint64_t msg_info_size) {
    msg_info_size_ = msg_info_size;
  }

  static constexpr int32_t kRWLockFree = 0;
  static constexpr int32_t kWriteExclusive = -1;
  static constexpr int32_t kMaxTryLockTimes = 5;

 private:
  // 尝试获取写锁：CAS 将 lock_num_ 从 0 改为 -1
  bool TryLockForWrite() {
    int32_t rw_lock_free = kRWLockFree;
    if (!lock_num_.compare_exchange_weak(rw_lock_free, kWriteExclusive,
                                         std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) {
      return false;
    }
    return true;
  }

  // 尝试获取读锁：lock_num_ 从 n CAS 到 n+1，最多重试 kMaxTryLockTimes 次
  bool TryLockForRead() {
    int32_t lock_num = lock_num_.load();
    if (lock_num < kRWLockFree) {
      // 正在被写
      return false;
    }
    int32_t try_times = 0;
    while (!lock_num_.compare_exchange_weak(lock_num, lock_num + 1,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
      ++try_times;
      if (try_times == kMaxTryLockTimes) {
        return false;
      }
      lock_num = lock_num_.load();
      if (lock_num < kRWLockFree) {
        return false;
      }
    }
    return true;
  }

  // 释放写锁：-1 -> 0
  void ReleaseWriteLock() { lock_num_.fetch_add(1, std::memory_order_release); }

  // 释放读锁：n -> n-1
  void ReleaseReadLock() { lock_num_.fetch_sub(1, std::memory_order_release); }

  std::atomic<int32_t> lock_num_ = {0};
  uint64_t msg_size_;
  uint64_t msg_info_size_;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_SHM_BLOCK_H_