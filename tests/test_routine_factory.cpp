#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <sys/syscall.h>
#include <unistd.h>

#include "minicyber/croutine/routine_factory.h"
#include "minicyber/data/data_dispatcher.h"
#include "minicyber/scheduler/scheduler.h"

namespace {

using minicyber::croutine::CreateRoutineFactory;
using minicyber::data::DataDispatcher;
using minicyber::data::DataVisitor;
using minicyber::data::VisitorConfig;
using minicyber::scheduler::Scheduler;
using minicyber::scheduler::SchedulerConf;

bool WaitForCount(std::condition_variable& ready, std::mutex& mutex,
                  const std::atomic<int>& count, int expected) {
  std::unique_lock<std::mutex> lock(mutex);
  return ready.wait_for(lock, std::chrono::seconds(2), [&] {
    return count.load(std::memory_order_acquire) >= expected;
  });
}

TEST(RoutineFactoryTest, SingleInputRunsProcAfterNotifierWake) {
  SchedulerConf config;
  config.thread_num = 1;
  Scheduler scheduler(config);
  auto visitor = std::make_shared<DataVisitor<int>>(VisitorConfig{9301, 4});

  std::atomic<int> processed{0};
  std::atomic<int> sum{0};
  std::atomic<pid_t> proc_tid{-1};
  std::condition_variable ready;
  std::mutex ready_mutex;
  const pid_t publisher_tid = static_cast<pid_t>(::syscall(SYS_gettid));

  auto factory = CreateRoutineFactory<int>(
      [&](const std::shared_ptr<int>& message) {
        sum.fetch_add(*message, std::memory_order_relaxed);
        proc_tid.store(static_cast<pid_t>(::syscall(SYS_gettid)),
                       std::memory_order_release);
        processed.fetch_add(1, std::memory_order_release);
        ready.notify_all();
      },
      visitor);
  ASSERT_EQ(factory.GetDataVisitor().get(), visitor.get());

  const uint64_t id = scheduler.CreateTask(factory, "factory_single", 5);
  ASSERT_NE(id, 0u);

  ASSERT_TRUE(DataDispatcher<int>::Instance()->Dispatch(
      9301, std::make_shared<int>(7)));
  ASSERT_TRUE(WaitForCount(ready, ready_mutex, processed, 1));
  EXPECT_EQ(sum.load(), 7);
  EXPECT_NE(proc_tid.load(std::memory_order_acquire), publisher_tid);

  ASSERT_TRUE(DataDispatcher<int>::Instance()->Dispatch(
      9301, std::make_shared<int>(11)));
  ASSERT_TRUE(WaitForCount(ready, ready_mutex, processed, 2));
  EXPECT_EQ(sum.load(), 18);
  scheduler.Shutdown();
}

TEST(RoutineFactoryTest, DualInputUsesPrimaryDrivenAllLatest) {
  SchedulerConf config;
  config.thread_num = 1;
  Scheduler scheduler(config);
  auto visitor = std::make_shared<DataVisitor<int, std::string>>(
      VisitorConfig{9302, 4}, VisitorConfig{9303, 4});

  std::atomic<int> processed{0};
  std::atomic<int> primary_value{-1};
  std::string secondary_value;
  std::mutex result_mutex;
  std::condition_variable ready;
  std::mutex ready_mutex;

  auto factory = CreateRoutineFactory<int, std::string>(
      [&](const std::shared_ptr<int>& primary,
          const std::shared_ptr<std::string>& secondary) {
        primary_value.store(*primary, std::memory_order_release);
        {
          std::lock_guard<std::mutex> lock(result_mutex);
          secondary_value = *secondary;
        }
        processed.fetch_add(1, std::memory_order_release);
        ready.notify_all();
      },
      visitor);

  const uint64_t id = scheduler.CreateTask(factory, "factory_dual", 5);
  ASSERT_NE(id, 0u);

  // 副通道 Buffer 会被填充，但没有 Visitor notifier，因此 Dispatch 返回 false
  // 表示没有协程唤醒；随后主通道到达必须仍能读取这一条最新值。
  EXPECT_FALSE(DataDispatcher<std::string>::Instance()->Dispatch(
      9303, std::make_shared<std::string>("vehicle-state")));
  EXPECT_EQ(processed.load(std::memory_order_acquire), 0);

  ASSERT_TRUE(DataDispatcher<int>::Instance()->Dispatch(
      9302, std::make_shared<int>(42)));
  ASSERT_TRUE(WaitForCount(ready, ready_mutex, processed, 1));
  EXPECT_EQ(primary_value.load(std::memory_order_acquire), 42);
  {
    std::lock_guard<std::mutex> lock(result_mutex);
    EXPECT_EQ(secondary_value, "vehicle-state");
  }
  scheduler.Shutdown();
}

}  // namespace
