#include <gtest/gtest.h>

#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "minicyber/croutine/croutine.h"
#include "minicyber/scheduler/scheduler_factory.h"

namespace minicyber {
namespace scheduler {

TEST(SchedulerFactoryTest, RejectsUnknownPolicy) {
  SchedulerConf conf;
  conf.policy = "unknown";
  EXPECT_EQ(SchedulerFactory::Create(conf), nullptr);
}

TEST(SchedulerFactoryTest, RejectsEmptyChoreographyZone) {
  SchedulerConf conf;
  conf.policy = "choreography";
  conf.choreography_processor_num = 1;
  EXPECT_EQ(SchedulerFactory::Create(conf), nullptr);

  conf.choreography_processor_num = 0;
  conf.pool_processor_num = 1;
  EXPECT_EQ(SchedulerFactory::Create(conf), nullptr);
}

TEST(SchedulerChoreographyTest, FactorySelectsChoreographyAndRoutesTarget) {
  SchedulerConf conf;
  conf.policy = "choreography";
  conf.choreography_processor_num = 2;
  conf.pool_processor_num = 1;
  auto sched = SchedulerFactory::Create(conf);
  ASSERT_NE(sched, nullptr);
  ASSERT_TRUE(sched->IsChoreography());
  ASSERT_EQ(sched->ProcessorCount(), 3u);

  std::atomic<pid_t> ran_on{-1};
  ASSERT_NE(sched->CreateTask(
                [&]() { ran_on.store(static_cast<pid_t>(syscall(SYS_gettid))); },
                "targeted", 5, 1),
            0u);
  for (int i = 0; i < 100 && ran_on.load() == -1; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(ran_on.load(), sched->ProcessorTid(1));
  sched->Shutdown();
}

TEST(SchedulerChoreographyTest, NotifyWakesTargetedDataWaitRoutine) {
  SchedulerConf conf;
  conf.policy = "choreography";
  conf.choreography_processor_num = 1;
  conf.pool_processor_num = 1;
  auto sched = SchedulerFactory::Create(conf);

  std::atomic<bool> resumed{false};
  const uint64_t id = sched->CreateTask(
      [&]() {
        CRoutine::Yield(RoutineState::DATA_WAIT);
        resumed.store(true);
      },
      "waiting", 5, 0);
  ASSERT_NE(id, 0u);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ASSERT_TRUE(sched->NotifyTask(id));
  for (int i = 0; i < 100 && !resumed.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(resumed.load());
  sched->Shutdown();
}

TEST(SchedulerChoreographyTest, NotifyWakesClassicPoolDataWaitRoutine) {
  SchedulerConf conf;
  conf.policy = "choreography";
  conf.choreography_processor_num = 1;
  conf.pool_processor_num = 1;
  auto sched = SchedulerFactory::Create(conf);
  ASSERT_NE(sched, nullptr);

  std::atomic<bool> resumed{false};
  const uint64_t id = sched->CreateTask(
      [&]() {
        CRoutine::Yield(RoutineState::DATA_WAIT);
        resumed.store(true);
      },
      "pool_wait", 5);
  ASSERT_NE(id, 0u);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ASSERT_TRUE(sched->NotifyTask(id));
  for (int i = 0; i < 100 && !resumed.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(resumed.load());
  sched->Shutdown();
}

TEST(SchedulerChoreographyTest, UnconfiguredTaskUsesClassicPool) {
  SchedulerConf conf;
  conf.policy = "choreography";
  conf.choreography_processor_num = 1;
  conf.pool_processor_num = 1;
  conf.choreography_tasks.push_back({"directed", 9, 0, true});
  auto sched = SchedulerFactory::Create(conf);
  ASSERT_NE(sched, nullptr);

  std::atomic<pid_t> directed_tid{-1};
  std::atomic<pid_t> pool_tid{-1};
  ASSERT_NE(sched->CreateTask(
                [&]() { directed_tid.store(static_cast<pid_t>(syscall(SYS_gettid))); },
                "directed", 1),
            0u);
  ASSERT_NE(sched->CreateTask(
                [&]() { pool_tid.store(static_cast<pid_t>(syscall(SYS_gettid))); },
                "pool", 1),
            0u);
  for (int i = 0; i < 200 &&
                  (directed_tid.load() == -1 || pool_tid.load() == -1);
       ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(directed_tid.load(), sched->ProcessorTid(0));
  EXPECT_EQ(pool_tid.load(), sched->ProcessorTid(1));
  sched->Shutdown();
}

TEST(SchedulerChoreographyTest, OutOfRangeProcessorUsesClassicPool) {
  SchedulerConf conf;
  conf.policy = "choreography";
  conf.choreography_processor_num = 1;
  conf.pool_processor_num = 1;
  conf.choreography_tasks.push_back({"fallback", 9, 99, true});
  auto sched = SchedulerFactory::Create(conf);
  ASSERT_NE(sched, nullptr);

  std::atomic<pid_t> ran_on{-1};
  ASSERT_NE(sched->CreateTask(
                [&]() { ran_on.store(static_cast<pid_t>(syscall(SYS_gettid))); },
                "fallback", 1),
            0u);
  for (int i = 0; i < 200 && ran_on.load() == -1; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(ran_on.load(), sched->ProcessorTid(1));
  sched->Shutdown();
}

TEST(SchedulerChoreographyStabilityTest, ConcurrentSubmissionDuringShutdown) {
  SchedulerConf conf;
  conf.policy = "choreography";
  conf.choreography_processor_num = 2;
  conf.pool_processor_num = 1;

  for (int round = 0; round < 8; ++round) {
    auto sched = SchedulerFactory::Create(conf);
    std::atomic<bool> start{false};
    std::vector<std::thread> submitters;
    for (int thread = 0; thread < 4; ++thread) {
      submitters.emplace_back([&]() {
        while (!start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        for (int i = 0; i < 64; ++i) {
          sched->CreateTask([]() {}, "concurrent", 5, i % 2);
        }
      });
    }
    start.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    sched->Shutdown();
    for (auto& submitter : submitters) {
      submitter.join();
    }
    EXPECT_EQ(sched->ProcessorCount(), 0u);
    EXPECT_EQ(sched->CreateTask([]() {}, "after_shutdown"), 0u);
  }
}

TEST(SchedulerChoreographyStabilityTest, PoliciesCanBeRecreatedInSequence) {
  for (const char* policy : {"classic", "choreography", "classic",
                             "choreography"}) {
    SchedulerConf conf;
    conf.policy = policy;
    conf.thread_num = 1;
    conf.choreography_processor_num = 1;
    conf.pool_processor_num = 1;
    auto sched = SchedulerFactory::Create(conf);
    EXPECT_EQ(sched->IsChoreography(),
              std::string(policy) == "choreography");
    std::atomic<bool> ran{false};
    ASSERT_NE(sched->CreateTask([&]() { ran.store(true); }, "policy", 5), 0u);
    for (int i = 0; i < 100 && !ran.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(ran.load());
    sched->Shutdown();
  }
}

}  // namespace scheduler
}  // namespace minicyber
