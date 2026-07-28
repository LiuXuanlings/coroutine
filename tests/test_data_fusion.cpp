#include "minicyber/data/data_visitor.h"

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
using minicyber::data::DataNotifier;
using minicyber::data::DataVisitor;
using minicyber::data::Notifier;
using minicyber::data::VisitorConfig;
using minicyber::scheduler::Scheduler;
using minicyber::scheduler::SchedulerConf;

// ---------------------------------------------------------------------------
// Unit tests for DataVisitor<M0, M1>::TryFetch (no coroutine context)
//
// AllLatest semantics under review:
//   - Fusion buffer is filled only on a PRIMARY (M0) dispatch, and only when
//     the secondary channel already has at least one value.
//   - The visitor's wake-up notifier is bound to M0 only; M1 dispatches do
//     NOT wake a parked coroutine.
// ---------------------------------------------------------------------------

TEST(DataVisitorFusionTest, TryFetchOnEmptyFusionReturnsFalse) {
  DataVisitor<int, int> dv(VisitorConfig{9201, 4}, VisitorConfig{9202, 4});
  std::shared_ptr<int> m0, m1;
  EXPECT_FALSE(dv.TryFetch(m0, m1));
}

TEST(DataVisitorFusionTest, OnlyPrimaryDispatchedReturnsFalse) {
  // M0 dispatch fires AllLatest's notifier, but M1 is empty → no fusion.
  DataVisitor<int, int> dv(VisitorConfig{9203, 4}, VisitorConfig{9204, 4});
  DataDispatcher<int>::Instance()->Dispatch(9203, std::make_shared<int>(11));
  std::shared_ptr<int> m0, m1;
  EXPECT_FALSE(dv.TryFetch(m0, m1));
}

TEST(DataVisitorFusionTest, OnlySecondaryDispatchedReturnsFalse) {
  // M1 dispatch does not trigger fusion (notifier is M0-only) and does not
  // fill the fusion buffer. TryFetch must report nothing.
  DataVisitor<int, int> dv(VisitorConfig{9205, 4}, VisitorConfig{9206, 4});
  DataDispatcher<int>::Instance()->Dispatch(9206, std::make_shared<int>(22));
  std::shared_ptr<int> m0, m1;
  EXPECT_FALSE(dv.TryFetch(m0, m1));
}

TEST(DataVisitorFusionTest, SecondaryThenPrimaryTriggersFusion) {
  // Dispatch M1 first, then M0. AllLatest's M0 notifier fires, finds M1's
  // latest, packs the pair into the fusion buffer.
  DataVisitor<int, int> dv(VisitorConfig{9207, 4}, VisitorConfig{9208, 4});
  DataDispatcher<int>::Instance()->Dispatch(9208, std::make_shared<int>(200));
  DataDispatcher<int>::Instance()->Dispatch(9207, std::make_shared<int>(100));
  std::shared_ptr<int> m0, m1;
  ASSERT_TRUE(dv.TryFetch(m0, m1));
  EXPECT_EQ(*m0, 100);
  EXPECT_EQ(*m1, 200);
}

TEST(DataVisitorFusionTest, RepeatedTryWithoutNewPrimaryReturnsFalse) {
  DataVisitor<int, int> dv(VisitorConfig{9209, 4}, VisitorConfig{9210, 4});
  DataDispatcher<int>::Instance()->Dispatch(9210, std::make_shared<int>(2));
  DataDispatcher<int>::Instance()->Dispatch(9209, std::make_shared<int>(1));
  std::shared_ptr<int> m0, m1;
  ASSERT_TRUE(dv.TryFetch(m0, m1));
  // No new M0 dispatch → fusion buffer not advanced → caught up.
  EXPECT_FALSE(dv.TryFetch(m0, m1));
}

TEST(DataVisitorFusionTest, NewPrimaryReFusesWithStaleSecondary) {
  // First fuse: M0=10, M1=20.
  DataVisitor<int, int> dv(VisitorConfig{9211, 4}, VisitorConfig{9212, 4});
  DataDispatcher<int>::Instance()->Dispatch(9212, std::make_shared<int>(20));
  DataDispatcher<int>::Instance()->Dispatch(9211, std::make_shared<int>(10));
  std::shared_ptr<int> m0, m1;
  ASSERT_TRUE(dv.TryFetch(m0, m1));
  EXPECT_EQ(*m0, 10);
  EXPECT_EQ(*m1, 20);

  // Only M0 advances. AllLatest reuses M1's stale latest (20).
  DataDispatcher<int>::Instance()->Dispatch(9211, std::make_shared<int>(99));
  ASSERT_TRUE(dv.TryFetch(m0, m1));
  EXPECT_EQ(*m0, 99);   // new primary
  EXPECT_EQ(*m1, 20);   // stale secondary
}

TEST(DataVisitorFusionTest, NewSecondaryAloneDoesNotReFuse) {
  // After a successful fuse, an M1-only dispatch must NOT produce a new fuse
  // (no M0 notifier fires). Confirms notifier is primary-only.
  DataVisitor<int, int> dv(VisitorConfig{9213, 4}, VisitorConfig{9214, 4});
  DataDispatcher<int>::Instance()->Dispatch(9214, std::make_shared<int>(20));
  DataDispatcher<int>::Instance()->Dispatch(9213, std::make_shared<int>(10));
  std::shared_ptr<int> m0, m1;
  ASSERT_TRUE(dv.TryFetch(m0, m1));

  DataDispatcher<int>::Instance()->Dispatch(9214, std::make_shared<int>(88));
  EXPECT_FALSE(dv.TryFetch(m0, m1));
}

TEST(DataVisitorFusionTest, RegisterNotifyCallbackFiresOnPrimaryOnly) {
  DataVisitor<int, int> dv(VisitorConfig{9215, 4}, VisitorConfig{9216, 4});
  std::atomic<int> counter{0};
  dv.RegisterNotifyCallback([&counter]() {
    counter.fetch_add(1, std::memory_order_relaxed);
  });

  // M1 dispatch: AllLatest has no notifier on M1; visitor notifier is on M0.
  // Counter must stay 0.
  DataDispatcher<int>::Instance()->Dispatch(9216, std::make_shared<int>(1));
  EXPECT_EQ(counter.load(), 0);

  // M0 dispatch: AllLatest's notifier + visitor's notifier both fire.
  DataDispatcher<int>::Instance()->Dispatch(9215, std::make_shared<int>(2));
  EXPECT_EQ(counter.load(), 1);
}

TEST(DataVisitorFusionTest, ChannelIdAccessors) {
  DataVisitor<int, int> dv(VisitorConfig{9217, 4}, VisitorConfig{9218, 4});
  EXPECT_EQ(dv.primary_channel_id(), 9217u);
  EXPECT_EQ(dv.secondary_channel_id(), 9218u);
  EXPECT_EQ(dv.channel_id(), 9217u);  // primary, for compatibility
}

// ---------------------------------------------------------------------------
// Integration tests (Scheduler + CRoutine)
// ---------------------------------------------------------------------------

TEST(DataVisitorFusionTest, FetchParksUntilSecondaryThenPrimary) {
  SchedulerConf conf;
  conf.thread_num = 1;
  Scheduler sched(conf);

  DataVisitor<int, int> dv(VisitorConfig{9219, 4}, VisitorConfig{9220, 4});
  std::atomic<uint64_t> task_id{0};
  dv.RegisterNotifyCallback([&]() {
    uint64_t id = task_id.load();
    if (id != 0) sched.NotifyTask(id);
  });

  std::atomic<bool> fetched{false};
  std::atomic<int> v0{-1};
  std::atomic<int> v1{-1};

  uint64_t id = sched.CreateTask(
      [&]() {
        std::shared_ptr<int> m0, m1;
        dv.Fetch(m0, m1);
        v0.store(*m0);
        v1.store(*m1);
        fetched.store(true);
      },
      "fusion_consumer", 5);
  task_id.store(id);

  // Let the coroutine start, TryFetch, and park in DATA_WAIT.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(fetched.load());

  // Dispatch M1 only — notifier is M0-only, coroutine must stay parked.
  DataDispatcher<int>::Instance()->Dispatch(9220, std::make_shared<int>(8));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(fetched.load()) << "M1-only dispatch must not wake the coroutine";

  // Dispatch M0 — AllLatest fills fusion buffer, then visitor notifier wakes
  // the coroutine. Fetch returns with the aligned pair.
  DataDispatcher<int>::Instance()->Dispatch(9219, std::make_shared<int>(7));
  for (int i = 0; i < 100 && !fetched.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(fetched.load());
  EXPECT_EQ(v0.load(), 7);
  EXPECT_EQ(v1.load(), 8);

  sched.Shutdown();
}

TEST(DataVisitorFusionTest, FetchReFusesAcrossMultiplePrimaryDispatches) {
  // Two Fetch calls. First parks until M1 then M0. Second parks until the
  // next M0 dispatch (M1 stays stale-but-present).
  SchedulerConf conf;
  conf.thread_num = 1;
  Scheduler sched(conf);

  DataVisitor<int, int> dv(VisitorConfig{9221, 4}, VisitorConfig{9222, 4});
  std::atomic<uint64_t> task_id{0};
  dv.RegisterNotifyCallback([&]() {
    uint64_t id = task_id.load();
    if (id != 0) sched.NotifyTask(id);
  });

  std::atomic<int> sum0{0};
  std::atomic<int> sum1{0};
  std::atomic<int> fetches{0};

  uint64_t id = sched.CreateTask(
      [&]() {
        std::shared_ptr<int> m0, m1;
        dv.Fetch(m0, m1);
        sum0.fetch_add(*m0);
        sum1.fetch_add(*m1);
        fetches.fetch_add(1);
        dv.Fetch(m0, m1);
        sum0.fetch_add(*m0);
        sum1.fetch_add(*m1);
        fetches.fetch_add(1);
      },
      "two_fuse_consumer", 5);
  task_id.store(id);

  // Prime M1, then M0 to release the first Fetch.
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  ASSERT_EQ(fetches.load(), 0);
  DataDispatcher<int>::Instance()->Dispatch(9222, std::make_shared<int>(100));
  DataDispatcher<int>::Instance()->Dispatch(9221, std::make_shared<int>(1));
  for (int i = 0; i < 100 && fetches.load() < 1; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(fetches.load(), 1);
  EXPECT_EQ(sum0.load(), 1);
  EXPECT_EQ(sum1.load(), 100);

  // Second Fetch parks. Only a new M0 dispatch releases it (M1 stays stale).
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_EQ(fetches.load(), 1);
  DataDispatcher<int>::Instance()->Dispatch(9221, std::make_shared<int>(2));
  for (int i = 0; i < 100 && fetches.load() < 2; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(fetches.load(), 2);
  EXPECT_EQ(sum0.load(), 3);    // 1 + 2
  EXPECT_EQ(sum1.load(), 200);  // 100 + 100 (stale M1 reused)

  sched.Shutdown();
}

TEST(DataVisitorFusionTest, FetchParksAgainWhenPrimaryArrivesBeforeSecondary) {
  // M0 arrives while M1 is empty: AllLatest's callback returns early, fusion
  // buffer stays empty, coroutine wakes (M0 notifier fired), TryFetch fails,
  // coroutine parks again. A subsequent M0 dispatch (after M1 has data)
  // releases it.
  SchedulerConf conf;
  conf.thread_num = 1;
  Scheduler sched(conf);

  DataVisitor<int, int> dv(VisitorConfig{9223, 4}, VisitorConfig{9224, 4});
  std::atomic<uint64_t> task_id{0};
  dv.RegisterNotifyCallback([&]() {
    uint64_t id = task_id.load();
    if (id != 0) sched.NotifyTask(id);
  });

  std::atomic<bool> fetched{false};
  std::atomic<int> v0{-1};
  std::atomic<int> v1{-1};

  uint64_t id = sched.CreateTask(
      [&]() {
        std::shared_ptr<int> m0, m1;
        dv.Fetch(m0, m1);
        v0.store(*m0);
        v1.store(*m1);
        fetched.store(true);
      },
      "fusion_wait_secondary", 5);
  task_id.store(id);

  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  EXPECT_FALSE(fetched.load());

  // M0 first — M1 empty, fusion buffer stays empty, coroutine re-parks.
  DataDispatcher<int>::Instance()->Dispatch(9223, std::make_shared<int>(5));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(fetched.load()) << "M0 with empty M1 must re-park, not return";

  // M1 arrives — but no M0 notifier fires, coroutine stays parked.
  DataDispatcher<int>::Instance()->Dispatch(9224, std::make_shared<int>(9));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(fetched.load()) << "M1 dispatch must not wake the coroutine";

  // Second M0 dispatch — now M1 has data, fusion succeeds.
  DataDispatcher<int>::Instance()->Dispatch(9223, std::make_shared<int>(6));
  for (int i = 0; i < 100 && !fetched.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(fetched.load());
  EXPECT_EQ(v0.load(), 6);   // latest M0 (second dispatch), not the first
  EXPECT_EQ(v1.load(), 9);

  sched.Shutdown();
}

}  // namespace
