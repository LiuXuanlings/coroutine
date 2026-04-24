#ifndef MINICYBER_BASE_WAIT_STRATEGY_H_
#define MINICYBER_BASE_WAIT_STRATEGY_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace minicyber {

// =============================================================================
// 等待策略基类
// 用于 BoundedQueue 等并发容器的阻塞/非阻塞策略
// =============================================================================
class WaitStrategy {
 public:
  virtual ~WaitStrategy() = default;

  // 唤醒一个等待者
  virtual void NotifyOne() {}

  // 唤醒所有等待者
  virtual void BreakAllWait() {}

  // 空等待（当队列空/满时调用）
  // 返回 true 表示继续尝试，false 表示超时或被中断
  virtual bool EmptyWait() = 0;
};

// =============================================================================
// 阻塞等待策略：使用 mutex + condition_variable
// 适用于低CPU占用、高延迟容忍的场景
// =============================================================================
class BlockWaitStrategy : public WaitStrategy {
 public:
  BlockWaitStrategy() = default;

  void NotifyOne() override { cv_.notify_one(); }

  bool EmptyWait() override {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock);
    return true;
  }

  void BreakAllWait() override { cv_.notify_all(); }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
};

// =============================================================================
// 睡眠等待策略：固定时长睡眠
// 适用于对实时性要求不高的场景，避免忙等待
// =============================================================================
class SleepWaitStrategy : public WaitStrategy {
 public:
  SleepWaitStrategy() = default;
  explicit SleepWaitStrategy(uint64_t sleep_time_us)
      : sleep_time_us_(sleep_time_us) {}

  bool EmptyWait() override {
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_time_us_));
    return true;
  }

  void SetSleepTimeMicroSeconds(uint64_t sleep_time_us) {
    sleep_time_us_ = sleep_time_us;
  }

 private:
  uint64_t sleep_time_us_ = 10000;  // 默认 10ms
};

// =============================================================================
// 让出等待策略：立即让出CPU时间片
// 适用于低延迟场景，但会增加CPU上下文切换开销
// =============================================================================
class YieldWaitStrategy : public WaitStrategy {
 public:
  YieldWaitStrategy() = default;

  bool EmptyWait() override {
    std::this_thread::yield();
    return true;
  }
};

// =============================================================================
// 忙自旋等待策略：立即返回
// 适用于极高性能场景，但会消耗大量CPU
// =============================================================================
class BusySpinWaitStrategy : public WaitStrategy {
 public:
  BusySpinWaitStrategy() = default;

  bool EmptyWait() override { return true; }
};

// =============================================================================
// 带超时阻塞策略：阻塞但有最大等待时间
// 适用于需要防止无限等待的场景
// =============================================================================
class TimeoutBlockWaitStrategy : public WaitStrategy {
 public:
  TimeoutBlockWaitStrategy() = default;
  explicit TimeoutBlockWaitStrategy(uint64_t timeout_ms)
      : time_out_(std::chrono::milliseconds(timeout_ms)) {}

  void NotifyOne() override { cv_.notify_one(); }

  bool EmptyWait() override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (cv_.wait_for(lock, time_out_) == std::cv_status::timeout) {
      return false;
    }
    return true;
  }

  void BreakAllWait() override { cv_.notify_all(); }

  void SetTimeout(uint64_t timeout_ms) {
    time_out_ = std::chrono::milliseconds(timeout_ms);
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::chrono::milliseconds time_out_;
};

}  // namespace minicyber

#endif  // MINICYBER_BASE_WAIT_STRATEGY_H_
