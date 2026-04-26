#ifndef MINICYBER_BASE_ATOMIC_RW_LOCK_H_
#define MINICYBER_BASE_ATOMIC_RW_LOCK_H_

#include <atomic>
#include <cstdint>
#include <thread>

#include "minicyber/base/macros.h"

namespace minicyber {

// =============================================================================
// 前置声明：锁守卫类
// =============================================================================
template <typename RWLock>
class ReadLockGuard;

template <typename RWLock>
class WriteLockGuard;

// =============================================================================
// 无锁读写锁 (Lock-free Read-Write Lock)
//
// 核心设计：
// - lock_num_: 原子整数表示锁状态
//   - 0 (RW_LOCK_FREE): 未锁定
//   - -1 (WRITE_EXCLUSIVE): 写锁独占
//   - >0: 当前活跃的读者数量
//
// - write_lock_wait_num_: 等待获取写锁的线程数
//   用于实现写优先策略（write_first_ = true）
//
// 获取读锁流程：
//   1. 检查是否有写者等待（write_first_ 模式）
//   2. CAS 将 lock_num 从 n 改为 n+1
//
// 获取写锁流程：
//   1. 增加 write_lock_wait_num_
//   2. CAS 将 lock_num 从 0 改为 -1
//   3. 减少 write_lock_wait_num_
//
// 优化：自旋 MAX_RETRY_TIMES 次后 yield，避免 CPU 空转
// =============================================================================
class AtomicRWLock {
  // 允许锁守卫访问私有方法
  friend class ReadLockGuard<AtomicRWLock>;
  friend class WriteLockGuard<AtomicRWLock>;

 public:
  static constexpr int32_t RW_LOCK_FREE = 0;        // 未锁定状态
  static constexpr int32_t WRITE_EXCLUSIVE = -1;    // 写锁独占状态
  static constexpr uint32_t MAX_RETRY_TIMES = 5;    // 自旋次数上限

  AtomicRWLock() = default;
  explicit AtomicRWLock(bool write_first) : write_first_(write_first) {}

  // 禁止拷贝和赋值
  AtomicRWLock(const AtomicRWLock&) = delete;
  AtomicRWLock& operator=(const AtomicRWLock&) = delete;

 private:
  // 获取读锁（仅供 ReadLockGuard 使用）
  inline void ReadLock();
  // 获取写锁（仅供 WriteLockGuard 使用）
  inline void WriteLock();
  // 释放读锁
  inline void ReadUnlock();
  // 释放写锁
  inline void WriteUnlock();

 private:
  // 等待写锁的线程数（用于写优先策略）
  alignas(CACHELINE_SIZE) std::atomic<uint32_t> write_lock_wait_num_{0};
  // 锁状态：-1=写锁, 0=空闲, >0=读者数
  alignas(CACHELINE_SIZE) std::atomic<int32_t> lock_num_{0};
  // 是否优先处理写者（避免写饥饿）
  bool write_first_ = true;
};

// =============================================================================
// 获取读锁
// =============================================================================
inline void AtomicRWLock::ReadLock() {
  uint32_t retry_times = 0;
  int32_t lock_num = lock_num_.load(std::memory_order_relaxed);

  if (write_first_) {
    // 写优先模式：如果有写者等待，读者需要等待
    do {
      // 等待条件：有写锁（lock_num < 0）或有写者等待
      while (lock_num < RW_LOCK_FREE || write_lock_wait_num_.load(std::memory_order_acquire) > 0) {
        if (++retry_times == MAX_RETRY_TIMES) {
          // 让出 CPU，避免忙等
          std::this_thread::yield();
          retry_times = 0;
        }
        lock_num = lock_num_.load(std::memory_order_relaxed);
      }
    } while (!lock_num_.compare_exchange_weak(lock_num, lock_num + 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed));
  } else {
    // 读优先模式：只要没有写锁就可以读（可能导致写饥饿）
    do {
      while (lock_num < RW_LOCK_FREE) {
        if (++retry_times == MAX_RETRY_TIMES) {
          std::this_thread::yield();
          retry_times = 0;
        }
        lock_num = lock_num_.load(std::memory_order_relaxed);
      }
    } while (!lock_num_.compare_exchange_weak(lock_num, lock_num + 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed));
  }
}

// =============================================================================
// 获取写锁
// =============================================================================
inline void AtomicRWLock::WriteLock() {
  int32_t rw_lock_free = RW_LOCK_FREE;
  uint32_t retry_times = 0;

  // 增加等待写锁计数，阻止新读者进入（写优先模式）
  write_lock_wait_num_.fetch_add(1, std::memory_order_release);

  // CAS 尝试获取写锁
  while (!lock_num_.compare_exchange_weak(rw_lock_free, WRITE_EXCLUSIVE,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed)) {
    // CAS 失败后 rw_lock_free 会被更新，需要重置
    rw_lock_free = RW_LOCK_FREE;
    if (++retry_times == MAX_RETRY_TIMES) {
      std::this_thread::yield();
      retry_times = 0;
    }
  }

  // 获取写锁成功，减少等待计数
  write_lock_wait_num_.fetch_sub(1, std::memory_order_release);
}

// =============================================================================
// 释放读锁
// =============================================================================
inline void AtomicRWLock::ReadUnlock() {
  lock_num_.fetch_sub(1, std::memory_order_release);
}

// =============================================================================
// 释放写锁
// =============================================================================
inline void AtomicRWLock::WriteUnlock() {
  // 写锁状态是 -1，释放时加 1 回到 0
  lock_num_.fetch_add(1, std::memory_order_release);
}

}  // namespace minicyber

#endif  // MINICYBER_BASE_ATOMIC_RW_LOCK_H_
