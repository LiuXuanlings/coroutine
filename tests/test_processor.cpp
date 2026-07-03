#include <gtest/gtest.h>
#include "minicyber/scheduler/processor.h"
#include "minicyber/croutine/croutine.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace minicyber {
namespace scheduler {

// ----------------------------------------------------------------------
// MockContext: 控制 NextRoutine/Wait 行为以测试 Processor 调度循环
// ----------------------------------------------------------------------
class MockContext : public ProcessorContext {
 public:
  void Enqueue(std::shared_ptr<CRoutine> cr) {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      queue_.push_back(cr);
    }
    cv_.notify_one();
    wait_count_.fetch_add(1);
  }

  std::shared_ptr<CRoutine> NextRoutine() override {
    std::lock_guard<std::mutex> lock(mtx_);
    if (queue_.empty()) return nullptr;
    auto cr = queue_.front();
    queue_.pop_front();
    return cr;
  }

  void Wait() override {
    wait_called_.fetch_add(1);
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this] { return !queue_.empty() || stop_.load(); });
  }

  void Shutdown() override {
    stop_.store(true);
    cv_.notify_all();
  }

  int WaitCalledCount() const { return wait_called_.load(); }

 private:
  std::deque<std::shared_ptr<CRoutine>> queue_;
  mutable std::mutex mtx_;
  std::condition_variable cv_;
  std::atomic<int> wait_called_{0};  // 记录Wait()方法被调用次数，供测试判断调度线程是否阻塞
  std::atomic<int> wait_count_{0};   // 记录Enqueue入队并唤醒条件变量的总次数，预留测试埋点
};

// ----------------------------------------------------------------------
// 测试 1：未 BindContext 时 Stop 不崩溃
// ----------------------------------------------------------------------
TEST(ProcessorTest, StopWithoutBindNoCrash) {
  Processor proc;
  proc.Stop();  // 应直接返回，不崩溃
}

// ----------------------------------------------------------------------
// 测试 2：Processor 正确执行入队协程
// ----------------------------------------------------------------------
TEST(ProcessorTest, RunsEnqueuedRoutines) {
  auto ctx = std::make_shared<MockContext>();
  Processor proc;
  proc.BindContext(ctx);

  std::atomic<int> counter{0};
  // 入队 3 个协程，每个递增 counter
  for (int i = 0; i < 3; ++i) {
    ctx->Enqueue(std::make_shared<CRoutine>([&]() { counter.fetch_add(1); }));
  }

  // 等待所有协程执行完
  while (counter.load() < 3) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  proc.Stop();
  EXPECT_EQ(counter.load(), 3);
}

// ----------------------------------------------------------------------
// 测试 3：队列空时调用 Wait
// ----------------------------------------------------------------------
TEST(ProcessorTest, WaitsWhenNoRoutine) {
  auto ctx = std::make_shared<MockContext>();
  Processor proc;
  proc.BindContext(ctx);

  // 等待 Processor 进入 Wait（队列为空时会调用 Wait）
  while (ctx->WaitCalledCount() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GT(ctx->WaitCalledCount(), 0);

  proc.Stop();
}

// ----------------------------------------------------------------------
// 测试 4：Tid 在 BindContext 后可用
// ----------------------------------------------------------------------
TEST(ProcessorTest, TidAvailableAfterBind) {
  auto ctx = std::make_shared<MockContext>();
  Processor proc;
  proc.BindContext(ctx);

  // Tid() 自旋等待直到就绪，应返回正值
  pid_t tid = proc.Tid().load();
  EXPECT_GT(tid, 0);

  proc.Stop();
}

// ----------------------------------------------------------------------
// 测试 5：Stop 唤醒阻塞在 Wait 的线程并退出
// ----------------------------------------------------------------------
TEST(ProcessorTest, StopUnblocksWait) {
  auto ctx = std::make_shared<MockContext>();
  Processor proc;
  proc.BindContext(ctx);

  // 确保 Processor 进入 Wait
  while (ctx->WaitCalledCount() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  proc.Stop();
  // Stop 返回后线程已 join，若卡住会超时失败
  SUCCEED();
}

// ----------------------------------------------------------------------
// 测试 6：ProcSnapshot 的 processor_id 等于 TID
// ----------------------------------------------------------------------
TEST(ProcessorTest, SnapshotProcessorIdMatchesTid) {
  auto ctx = std::make_shared<MockContext>();
  Processor proc;
  proc.BindContext(ctx);

  pid_t tid = proc.Tid().load();
  EXPECT_EQ(proc.ProcSnapshot()->processor_id.load(), tid);

  proc.Stop();
}

}  // namespace scheduler
}  // namespace minicyber
