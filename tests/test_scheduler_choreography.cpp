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

}  // namespace scheduler
}  // namespace minicyber
