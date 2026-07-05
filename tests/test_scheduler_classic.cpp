#include <gtest/gtest.h>
#include "minicyber/scheduler/scheduler.h"
#include "minicyber/croutine/croutine.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace minicyber {
namespace scheduler {

// ----------------------------------------------------------------------
// 测试 1：创建任务并执行
// ----------------------------------------------------------------------
TEST(SchedulerTest, CreateTaskAndExecute) {
  SchedulerConf conf;
  conf.thread_num = 2;
  {
    Scheduler sched(conf);

    std::atomic<int> counter{0};
    uint64_t id = sched.CreateTask([&]() { counter.fetch_add(1); }, "task1", 5);
    EXPECT_NE(id, 0u);

    // 等待执行
    for (int i = 0; i < 100 && counter.load() < 1; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(counter.load(), 1);

    sched.Shutdown();
  }
}

// ----------------------------------------------------------------------
// 测试 2：多个任务都被执行
// ----------------------------------------------------------------------
TEST(SchedulerTest, MultipleTasksAllExecute) {
  SchedulerConf conf;
  conf.thread_num = 2;
  {
    Scheduler sched(conf);

    std::atomic<int> counter{0};
    for (int i = 0; i < 10; ++i) {
      sched.CreateTask([&]() { counter.fetch_add(1); }, "task", 5);
    }

    for (int i = 0; i < 200 && counter.load() < 10; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(counter.load(), 10);

    sched.Shutdown();
  }
}

// ----------------------------------------------------------------------
// 测试 3：NotifyTask 唤醒 DATA_WAIT 的协程
// ----------------------------------------------------------------------
TEST(SchedulerTest, NotifyTaskWakesDataWait) {
  SchedulerConf conf;
  conf.thread_num = 1;
  {
    Scheduler sched(conf);

    std::atomic<bool> resumed{false};
    uint64_t id = sched.CreateTask(
        [&]() {
          // 模拟数据未就绪，让出进入 DATA_WAIT
          CRoutine::Yield(RoutineState::DATA_WAIT);
          // 被唤醒后继续执行
          resumed.store(true);
        },
        "data_wait_task", 5);

    // 等待协程进入 DATA_WAIT
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 通知数据就绪，应唤醒协程
    EXPECT_TRUE(sched.NotifyTask(id));

    // 等待协程恢复执行
    for (int i = 0; i < 200 && !resumed.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(resumed.load());

    sched.Shutdown();
  }
}

// ----------------------------------------------------------------------
// 测试 4：Shutdown 停止所有 Processor
// ----------------------------------------------------------------------
TEST(SchedulerTest, ShutdownStopsAllProcessors) {
  SchedulerConf conf;
  conf.thread_num = 4;
  {
    Scheduler sched(conf);
    EXPECT_EQ(sched.ProcessorCount(), 4u);

    sched.Shutdown();
    // 这里仅验证不崩溃
    SUCCEED();
  }
}

// ----------------------------------------------------------------------
// 测试 5：GetThis 返回当前线程创建的 Scheduler
// ----------------------------------------------------------------------
TEST(SchedulerTest, GetThisReturnsCurrentScheduler) {
  SchedulerConf conf;
  conf.thread_num = 1;
  {
    Scheduler sched(conf);
    EXPECT_EQ(Scheduler::GetThis(), &sched);
    sched.Shutdown();
  }
  // Shutdown 后 GetThis 应为 nullptr
  EXPECT_EQ(Scheduler::GetThis(), nullptr);
}

// ----------------------------------------------------------------------
// 测试 6：多 Processor 负载分担
// ----------------------------------------------------------------------
TEST(SchedulerTest, MultipleProcessorsLoadBalance) {
  SchedulerConf conf;
  conf.thread_num = 3;
  {
    Scheduler sched(conf);

    std::atomic<int> counter{0};
    // 创建 30 个任务，验证多 Processor 共同执行
    for (int i = 0; i < 30; ++i) {
      sched.CreateTask([&]() { counter.fetch_add(1); }, "task", 5);
    }

    for (int i = 0; i < 500 && counter.load() < 30; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(counter.load(), 30);

    sched.Shutdown();
  }
}

}  // namespace scheduler
}  // namespace minicyber