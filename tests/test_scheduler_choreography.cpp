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

TEST(SchedulerChoreographyTest, FactorySelectsChoreographyAndRoutesTarget) {
  SchedulerConf conf;
  conf.policy = "choreography";
  conf.choreography_processor_num = 2;
  auto sched = SchedulerFactory::Create(conf);
  ASSERT_TRUE(sched->IsChoreography());
  ASSERT_EQ(sched->ProcessorCount(), 2u);

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

TEST(SchedulerChoreographyStabilityTest, ConcurrentSubmissionDuringShutdown) {
  SchedulerConf conf;
  conf.policy = "choreography";
  conf.choreography_processor_num = 2;

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
