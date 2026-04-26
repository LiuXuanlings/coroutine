#ifndef MINICYBER_BASE_RW_LOCK_GUARD_H_
#define MINICYBER_BASE_RW_LOCK_GUARD_H_

#include <cstdint>

namespace minicyber {

// =============================================================================
// 读锁守卫 (Read Lock Guard)
// RAII 封装：构造函数获取读锁，析构函数释放读锁
// =============================================================================
template <typename RWLock>
class ReadLockGuard {
 public:
  explicit ReadLockGuard(RWLock& lock) : rw_lock_(lock) {
    rw_lock_.ReadLock();
  }

  ~ReadLockGuard() {
    rw_lock_.ReadUnlock();
  }

  // 禁止拷贝和赋值
  ReadLockGuard(const ReadLockGuard& other) = delete;
  ReadLockGuard& operator=(const ReadLockGuard& other) = delete;

 private:
  RWLock& rw_lock_;
};

// =============================================================================
// 写锁守卫 (Write Lock Guard)
// RAII 封装：构造函数获取写锁，析构函数释放写锁
// =============================================================================
template <typename RWLock>
class WriteLockGuard {
 public:
  explicit WriteLockGuard(RWLock& lock) : rw_lock_(lock) {
    rw_lock_.WriteLock();
  }

  ~WriteLockGuard() {
    rw_lock_.WriteUnlock();
  }

  // 禁止拷贝和赋值
  WriteLockGuard(const WriteLockGuard& other) = delete;
  WriteLockGuard& operator=(const WriteLockGuard& other) = delete;

 private:
  RWLock& rw_lock_;
};

}  // namespace minicyber

#endif  // MINICYBER_BASE_RW_LOCK_GUARD_H_
