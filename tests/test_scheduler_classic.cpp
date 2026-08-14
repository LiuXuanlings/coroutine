#include <gtest/gtest.h>
#include "minicyber/proto/scheduler_conf.pb.h"
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
    EXPECT_TRUE(sched.IsStopped());
    EXPECT_EQ(sched.ProcessorCount(), 0u);
    EXPECT_EQ(sched.CreateTask([]() {}, "after_shutdown"), 0u);
    EXPECT_FALSE(sched.NotifyTask(1));
  }
}

TEST(SchedulerTest, AppliesConfiguredProcessorPolicy) {
  SchedulerConf conf;
  conf.thread_num = 1;
  conf.processor_policy = "SCHED_UNKNOWN";
  conf.processor_prio = 0;
  Scheduler sched(conf);

  std::atomic<bool> ran{false};
  ASSERT_NE(sched.CreateTask([&]() { ran.store(true); }, "configured", 5), 0u);
  for (int i = 0; i < 100 && !ran.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(ran.load());
  sched.Shutdown();
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

TEST(SchedulerConfTest, ParsesClassicGroupsAndTasksFromProtobuf) {
  proto::SchedulerConf proto_conf;
  proto_conf.set_policy("classic");
  auto* group = proto_conf.mutable_classic_conf()->add_groups();
  group->set_name("perception");
  group->set_processor_num(2);
  group->set_affinity("range");
  group->set_cpuset("1-2");
  group->set_processor_policy("SCHED_OTHER");
  group->set_processor_prio(0);
  auto* task = group->add_tasks();
  task->set_name("perception_task");
  task->set_prio(17);

  SchedulerConf conf;
  ASSERT_TRUE(SchedulerConf::FromProto(proto_conf, &conf));
  ASSERT_EQ(conf.classic_groups.size(), 1u);
  const auto& parsed_group = conf.classic_groups.front();
  EXPECT_EQ(parsed_group.name, "perception");
  EXPECT_EQ(parsed_group.processor_num, 2u);
  EXPECT_EQ(parsed_group.cpuset, (std::vector<int>{1, 2}));
  ASSERT_EQ(parsed_group.tasks.size(), 1u);
  EXPECT_EQ(parsed_group.tasks.front().name, "perception_task");
  EXPECT_EQ(parsed_group.tasks.front().priority, 17u);
  EXPECT_EQ(parsed_group.tasks.front().group_name, "perception");
}

TEST(SchedulerTest, ClassicGroupSharesProcessorsAndConfiguredTaskQueue) {
  SchedulerConf conf;
  ClassicGroupConf group;
  group.name = "shared_group";
  group.processor_num = 2;
  group.tasks.push_back({"configured_task", 19, group.name});
  conf.classic_groups.push_back(group);
  Scheduler sched(conf);

  std::atomic<int> complete{0};
  for (int index = 0; index < 40; ++index) {
    ASSERT_NE(sched.CreateTask([&]() { complete.fetch_add(1); },
                               "configured_task", 0),
              0u);
  }
  for (int index = 0; index < 500 && complete.load() != 40; ++index) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(complete.load(), 40);
  sched.Shutdown();
}

TEST(SchedulerStabilityTest, ConcurrentSubmissionDuringShutdown) {
  SchedulerConf conf;
  conf.thread_num = 2;

  for (int round = 0; round < 8; ++round) {
    Scheduler sched(conf);
    std::atomic<bool> start{false};
    std::vector<std::thread> submitters;
    for (int thread = 0; thread < 4; ++thread) {
      submitters.emplace_back([&]() {
        while (!start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        for (int i = 0; i < 64; ++i) {
          sched.CreateTask([]() {}, "concurrent", 5);
        }
      });
    }
    start.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    sched.Shutdown();
    for (auto& submitter : submitters) {
      submitter.join();
    }
    EXPECT_TRUE(sched.IsStopped());
    EXPECT_EQ(sched.ProcessorCount(), 0u);
    EXPECT_EQ(sched.CreateTask([]() {}, "after_shutdown"), 0u);
  }
}

}  // namespace scheduler
}  // namespace minicyber
