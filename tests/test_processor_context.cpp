#include <gtest/gtest.h>
#include "minicyber/scheduler/processor_context.h"
#include "minicyber/croutine/croutine.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace minicyber {
namespace scheduler {

// ----------------------------------------------------------------------
// MockContext: 用于测试 ProcessorContext 接口契约
// ----------------------------------------------------------------------
// 行为：
//   - NextRoutine() 返回队列中下一个协程，队列空返回 nullptr
//   - Wait() 在条件变量上阻塞，直到有任务或 stop_ 为真
//   - Shutdown() 重写为 notify_all 以唤醒阻塞的 Wait()
// ----------------------------------------------------------------------
class MockContext : public ProcessorContext {
 public:
  void Enqueue(std::shared_ptr<CRoutine> cr) {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      queue_.push_back(cr);
    }
    cv_.notify_one();
  }

  std::shared_ptr<CRoutine> NextRoutine() override {
    std::lock_guard<std::mutex> lock(mtx_);
    if (queue_.empty()) return nullptr;
    auto cr = queue_.front();
    queue_.pop_front();
    return cr;
  }

  void Wait() override {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this] { return !queue_.empty() || stop_.load(); });
  }

  void Shutdown() override {
    stop_.store(true);
    cv_.notify_all();
  }

  bool IsStopped() const { return stop_.load(); }

  size_t QueueSize() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return queue_.size();
  }

 private:
  std::deque<std::shared_ptr<CRoutine>> queue_;
  mutable std::mutex mtx_;
  std::condition_variable cv_;
};

// ----------------------------------------------------------------------
// 测试 1：Shutdown 设置 stop_ 标志
// ----------------------------------------------------------------------
TEST(ProcessorContextTest, ShutdownSetsStopFlag) {
  MockContext ctx;
  EXPECT_FALSE(ctx.IsStopped());
  ctx.Shutdown();
  EXPECT_TRUE(ctx.IsStopped());
}

// ----------------------------------------------------------------------
// 测试 2：NextRoutine 空队列返回 nullptr
// ----------------------------------------------------------------------
TEST(ProcessorContextTest, NextRoutineEmptyReturnsNull) {
  MockContext ctx;
  EXPECT_EQ(ctx.NextRoutine(), nullptr);
}

// ----------------------------------------------------------------------
// 测试 3：NextRoutine 入队后可取出
// ----------------------------------------------------------------------
TEST(ProcessorContextTest, NextRoutineReturnsEnqueued) {
  MockContext ctx;
  auto cr = std::make_shared<CRoutine>([]() {});
  ctx.Enqueue(cr);

  auto out = ctx.NextRoutine();
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out.get(), cr.get());
  EXPECT_EQ(ctx.NextRoutine(), nullptr);  // 取完应返回 nullptr
}

// ----------------------------------------------------------------------
// 测试 4：Wait 在 Shutdown 后解除阻塞
// ----------------------------------------------------------------------
TEST(ProcessorContextTest, WaitUnblocksOnShutdown) {
  MockContext ctx;

  // 在另一个线程上 Wait，主线程 Shutdown 唤醒它
  std::atomic<bool> wait_returned{false};
  std::thread waiter([&]() {
    ctx.Wait();
    wait_returned.store(true);
  });

  // 给 waiter 一点时间进入 Wait
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(wait_returned.load());

  ctx.Shutdown();
  waiter.join();
  EXPECT_TRUE(wait_returned.load());
}

// ----------------------------------------------------------------------
// 测试 5：Wait 在有任务时解除阻塞
// ----------------------------------------------------------------------
TEST(ProcessorContextTest, WaitUnblocksOnNewTask) {
  MockContext ctx;

  std::atomic<bool> wait_returned{false};
  std::thread waiter([&]() {
    ctx.Wait();
    wait_returned.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(wait_returned.load());

  // 入队一个任务应唤醒 Wait
  ctx.Enqueue(std::make_shared<CRoutine>([]() {}));
  waiter.join();
  EXPECT_TRUE(wait_returned.load());
}

}  // namespace scheduler
}  // namespace minicyber
