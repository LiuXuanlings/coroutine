#include "minicyber/data/data_fusion.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "minicyber/data/data_dispatcher.h"
#include "minicyber/data/data_notifier.h"
#include "minicyber/scheduler/scheduler.h"

namespace {

using minicyber::data::DataDispatcher;
using minicyber::data::DataFusion;
using minicyber::data::DataNotifier;
using minicyber::data::Notifier;
using minicyber::data::VisitorConfig;
using minicyber::scheduler::Scheduler;
using minicyber::scheduler::SchedulerConf;

// Helper: register a no-op notifier so Dispatch's Notify returns true.
static void RegisterNoopNotifier(uint64_t channel_id) {
  auto n = std::make_shared<Notifier>();
  n->callback = []() {};
  DataNotifier::Instance()->AddNotifier(channel_id, n);
}

// ---------------------------------------------------------------------------
// Unit tests (no coroutine context)
// ---------------------------------------------------------------------------

TEST(DataFusionTest, TryAllLatestOnEmptyChannelsReturnsFalse) {
  DataFusion<int, int> df(VisitorConfig{9101, 4}, VisitorConfig{9102, 4});
  std::shared_ptr<int> m1, m2;
  EXPECT_FALSE(df.TryAllLatest(m1, m2));
}

TEST(DataFusionTest, OnlyChannel1ReadyReturnsFalse) {
  RegisterNoopNotifier(9103);
  DataFusion<int, int> df(VisitorConfig{9103, 4}, VisitorConfig{9104, 4});
  DataDispatcher<int>::Instance()->Dispatch(9103, std::make_shared<int>(11));
  std::shared_ptr<int> m1, m2;
  EXPECT_FALSE(df.TryAllLatest(m1, m2));
}

TEST(DataFusionTest, OnlyChannel2ReadyReturnsFalse) {
  RegisterNoopNotifier(9106);
  DataFusion<int, int> df(VisitorConfig{9105, 4}, VisitorConfig{9106, 4});
  DataDispatcher<int>::Instance()->Dispatch(9106, std::make_shared<int>(22));
  std::shared_ptr<int> m1, m2;
  EXPECT_FALSE(df.TryAllLatest(m1, m2));
}

TEST(DataFusionTest, BothChannelsReadyReturnsLatestOfEach) {
  RegisterNoopNotifier(9107);
  RegisterNoopNotifier(9108);
  DataFusion<int, int> df(VisitorConfig{9107, 4}, VisitorConfig{9108, 4});
  DataDispatcher<int>::Instance()->Dispatch(9107, std::make_shared<int>(100));
  DataDispatcher<int>::Instance()->Dispatch(9108, std::make_shared<int>(200));
  std::shared_ptr<int> m1, m2;
  ASSERT_TRUE(df.TryAllLatest(m1, m2));
  EXPECT_EQ(*m1, 100);
  EXPECT_EQ(*m2, 200);
}

TEST(DataFusionTest, RepeatedTryWithoutNewDataReturnsFalse) {
  RegisterNoopNotifier(9109);
  RegisterNoopNotifier(9110);
  DataFusion<int, int> df(VisitorConfig{9109, 4}, VisitorConfig{9110, 4});
  DataDispatcher<int>::Instance()->Dispatch(9109, std::make_shared<int>(1));
  DataDispatcher<int>::Instance()->Dispatch(9110, std::make_shared<int>(2));
  std::shared_ptr<int> m1, m2;
  ASSERT_TRUE(df.TryAllLatest(m1, m2));
  // No new data on either channel -> caught up.
  EXPECT_FALSE(df.TryAllLatest(m1, m2));
}

TEST(DataFusionTest, NewDataOnOneChannelTriggersFuseWithStaleLatestOfOther) {
  // AllLatest semantics: a fuse fires whenever at least one channel advanced,
  // returning the newest of each channel (the other may be stale).
  RegisterNoopNotifier(9111);
  RegisterNoopNotifier(9112);
  DataFusion<int, int> df(VisitorConfig{9111, 4}, VisitorConfig{9112, 4});
  DataDispatcher<int>::Instance()->Dispatch(9111, std::make_shared<int>(10));
  DataDispatcher<int>::Instance()->Dispatch(9112, std::make_shared<int>(20));
  std::shared_ptr<int> m1, m2;
  ASSERT_TRUE(df.TryAllLatest(m1, m2));
  EXPECT_EQ(*m1, 10);
  EXPECT_EQ(*m2, 20);

  // Only ch2 advances. Next fuse fires with ch1's stale latest (10).
  DataDispatcher<int>::Instance()->Dispatch(9112, std::make_shared<int>(99));
  ASSERT_TRUE(df.TryAllLatest(m1, m2));
  EXPECT_EQ(*m1, 10);   // stale latest of ch1
  EXPECT_EQ(*m2, 99);   // new latest of ch2
}

TEST(DataFusionTest, ChannelIdAccessors) {
  DataFusion<int, int> df(VisitorConfig{9113, 4}, VisitorConfig{9114, 4});
  EXPECT_EQ(df.channel_id1(), 9113u);
  EXPECT_EQ(df.channel_id2(), 9114u);
}

// ---------------------------------------------------------------------------
// Integration test (Scheduler + CRoutine)
// ---------------------------------------------------------------------------

TEST(DataFusionTest, WaitForAllLatestParksUntilBothReady) {
  SchedulerConf conf;
  conf.thread_num = 1;
  Scheduler sched(conf);

  RegisterNoopNotifier(9115);
  RegisterNoopNotifier(9116);
  DataFusion<int, int> df(VisitorConfig{9115, 4}, VisitorConfig{9116, 4});

  std::atomic<uint64_t> task_id{0};
  df.RegisterWakeCallback([&]() {
    uint64_t id = task_id.load();
    if (id != 0) sched.NotifyTask(id);
  });

  std::atomic<bool> fused{false};
  std::atomic<int> v1{-1};
  std::atomic<int> v2{-1};

  uint64_t id = sched.CreateTask(
      [&]() {
        std::shared_ptr<int> m1, m2;
        df.WaitForAllLatest(m1, m2);
        v1.store(*m1);
        v2.store(*m2);
        fused.store(true);
      },
      "fusion_consumer", 5);
  task_id.store(id);

  // Let the coroutine start and park in DATA_WAIT.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(fused.load());

  // Dispatch ch1 only — coroutine wakes, re-checks, parks again (ch2 not ready).
  DataDispatcher<int>::Instance()->Dispatch(9115, std::make_shared<int>(7));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(fused.load()) << "coroutine must not return with only ch1 ready";

  // Dispatch ch2 — now both ready, coroutine returns.
  DataDispatcher<int>::Instance()->Dispatch(9116, std::make_shared<int>(8));
  for (int i = 0; i < 100 && !fused.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(fused.load());
  EXPECT_EQ(v1.load(), 7);
  EXPECT_EQ(v2.load(), 8);

  sched.Shutdown();
}

}  // namespace