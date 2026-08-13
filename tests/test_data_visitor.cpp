#include "minicyber/data/data_visitor.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "minicyber/croutine/croutine.h"
#include "minicyber/data/data_dispatcher.h"
#include "minicyber/data/data_notifier.h"
#include "minicyber/scheduler/scheduler.h"

namespace {

using minicyber::data::CacheBuffer;
using minicyber::data::ChannelBuffer;
using minicyber::data::DataDispatcher;
using minicyber::data::DataNotifier;
using minicyber::data::DataVisitor;
using minicyber::data::VisitorConfig;
using minicyber::scheduler::Scheduler;
using minicyber::scheduler::SchedulerConf;

// ---------------------------------------------------------------------------
// Unit tests for TryFetch (no coroutine context required)
// ---------------------------------------------------------------------------

TEST(DataVisitorTest, TryFetchOnEmptyBufferReturnsFalse) {
  DataVisitor<int> dv(VisitorConfig{9001, 4});
  std::shared_ptr<int> m = std::make_shared<int>(-1);
  EXPECT_FALSE(dv.TryFetch(m));
  EXPECT_EQ(*m, -1);  // untouched on failure
}

TEST(DataVisitorTest, TryFetchReturnsDispatchedMessage) {
  DataVisitor<int> dv(VisitorConfig{9002, 4});
  // Register a no-op notifier so Dispatch returns true (buffers are filled
  // regardless of Notify's return value, but we want the true path here).
  auto n = std::make_shared<minicyber::data::Notifier>();
  n->SetCallback([]() {});
  DataNotifier::Instance()->AddNotifier(9002, n);

  auto msg = std::make_shared<int>(42);
  ASSERT_TRUE(DataDispatcher<int>::Instance()->Dispatch(9002, msg));
  std::shared_ptr<int> out;
  ASSERT_TRUE(dv.TryFetch(out));
  EXPECT_EQ(*out, 42);
}

TEST(DataVisitorTest, TryFetchAdvancesIndexAndSecondCallReturnsFalseWhenCaughtUp) {
  DataVisitor<int> dv(VisitorConfig{9003, 4});
  auto n = std::make_shared<minicyber::data::Notifier>();
  n->SetCallback([]() {});
  DataNotifier::Instance()->AddNotifier(9003, n);

  DataDispatcher<int>::Instance()->Dispatch(9003, std::make_shared<int>(1));
  std::shared_ptr<int> out;
  ASSERT_TRUE(dv.TryFetch(out));
  EXPECT_EQ(*out, 1);
  // No new data since last fetch -> caught up.
  EXPECT_FALSE(dv.TryFetch(out));
}

TEST(DataVisitorTest, TryFetchAcrossMultipleDispatches) {
  DataVisitor<int> dv(VisitorConfig{9004, 4});
  auto n = std::make_shared<minicyber::data::Notifier>();
  n->SetCallback([]() {});
  DataNotifier::Instance()->AddNotifier(9004, n);

  // First dispatch, then TryFetch (consumer starts after producer).
  DataDispatcher<int>::Instance()->Dispatch(9004, std::make_shared<int>(10));
  std::shared_ptr<int> out;
  ASSERT_TRUE(dv.TryFetch(out));  // index=0 -> Tail=1, returns element at 1
  EXPECT_EQ(*out, 10);
  EXPECT_FALSE(dv.TryFetch(out));  // caught up

  // Second dispatch, then TryFetch again.
  DataDispatcher<int>::Instance()->Dispatch(9004, std::make_shared<int>(20));
  ASSERT_TRUE(dv.TryFetch(out));  // index=2 -> Tail=2, returns element at 2
  EXPECT_EQ(*out, 20);
  EXPECT_FALSE(dv.TryFetch(out));
}

TEST(DataVisitorTest, RegisterNotifyCallbackFiresOnDispatch) {
  DataVisitor<int> dv(VisitorConfig{9005, 4});
  std::atomic<int> counter{0};
  dv.RegisterNotifyCallback([&counter]() {
    counter.fetch_add(1, std::memory_order_relaxed);
  });
  DataDispatcher<int>::Instance()->Dispatch(9005, std::make_shared<int>(1));
  DataDispatcher<int>::Instance()->Dispatch(9005, std::make_shared<int>(2));
  EXPECT_EQ(counter.load(), 2);
}

TEST(DataVisitorTest, ChannelIdAndBufferAccessors) {
  DataVisitor<int> dv(VisitorConfig{9006, 4});
  EXPECT_EQ(dv.channel_id(), 9006u);
  // Buffer accessor exposes the underlying ChannelBuffer for diagnostics.
  EXPECT_EQ(dv.buffer().channel_id(), 9006u);
}

TEST(DataVisitorTest, DestructionUnregistersBufferAndNotifier) {
  constexpr uint64_t kChannel = 9009;
  std::atomic<int> callbacks{0};
  {
    DataVisitor<int> dv(VisitorConfig{kChannel, 4});
    dv.RegisterNotifyCallback(
        [&]() { callbacks.fetch_add(1, std::memory_order_relaxed); });
    ASSERT_TRUE(DataDispatcher<int>::Instance()->Dispatch(
        kChannel, std::make_shared<int>(1)));
    EXPECT_EQ(callbacks.load(), 1);
  }
  EXPECT_FALSE(DataDispatcher<int>::Instance()->Dispatch(
      kChannel, std::make_shared<int>(2)));
  EXPECT_EQ(callbacks.load(), 1);
}

// ---------------------------------------------------------------------------
// Integration test for Fetch (requires Scheduler + CRoutine context)
// ---------------------------------------------------------------------------

TEST(DataVisitorTest, FetchBlocksAndResumesOnDispatch) {
  SchedulerConf conf;
  conf.thread_num = 1;
  Scheduler sched(conf);

  DataVisitor<int> dv(VisitorConfig{9007, 4});
  // Wire the notifier callback to wake the coroutine via the scheduler:
  // the callback is fired by DataNotifier after Dispatch; it calls
  // NotifyTask to re-enqueue the parked coroutine.
  std::atomic<uint64_t> task_id{0};
  dv.RegisterNotifyCallback([&]() {
    uint64_t id = task_id.load();
    if (id != 0) {
      sched.NotifyTask(id);
    }
  });

  std::atomic<bool> fetched{false};
  std::atomic<int> fetched_value{-1};

  uint64_t id = sched.CreateTask(
      [&]() {
        std::shared_ptr<int> msg;
        // Fetch() will loop: TryFetch fails -> DATA_WAIT + Yield -> retry.
        dv.Fetch(msg);
        fetched_value.store(*msg);
        fetched.store(true);
      },
      "data_wait_consumer", 5);
  task_id.store(id);

  // Let the coroutine start, TryFetch, and park in DATA_WAIT.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(fetched.load());

  // Publish data: Dispatch fills the buffer and fires the notifier, which
  // calls NotifyTask -> the coroutine wakes and Fetch succeeds.
  DataDispatcher<int>::Instance()->Dispatch(9007, std::make_shared<int>(777));

  for (int i = 0; i < 100 && !fetched.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(fetched.load());
  EXPECT_EQ(fetched_value.load(), 777);

  sched.Shutdown();
}

TEST(DataVisitorTest, FetchRetriesAcrossMultipleWaits) {
  // Coroutine Fetches twice; first Fetch parks until first Dispatch, second
  // Fetch parks until second Dispatch. Verifies the TryFetch loop survives
  // multiple DATA_WAIT -> READY -> DATA_WAIT transitions.
  SchedulerConf conf;
  conf.thread_num = 1;
  Scheduler sched(conf);

  DataVisitor<int> dv(VisitorConfig{9008, 4});
  std::atomic<uint64_t> task_id{0};
  dv.RegisterNotifyCallback([&]() {
    uint64_t id = task_id.load();
    if (id != 0) sched.NotifyTask(id);
  });

  std::atomic<int> sum{0};
  std::atomic<int> fetches{0};

  uint64_t id = sched.CreateTask(
      [&]() {
        std::shared_ptr<int> m;
        dv.Fetch(m);
        sum.fetch_add(*m);
        fetches.fetch_add(1);
        dv.Fetch(m);
        sum.fetch_add(*m);
        fetches.fetch_add(1);
      },
      "two_fetch_consumer", 5);
  task_id.store(id);

  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  ASSERT_EQ(fetches.load(), 0);
  DataDispatcher<int>::Instance()->Dispatch(9008, std::make_shared<int>(100));
  for (int i = 0; i < 100 && fetches.load() < 1; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(fetches.load(), 1);

  // Second Fetch parks again.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_EQ(fetches.load(), 1);
  DataDispatcher<int>::Instance()->Dispatch(9008, std::make_shared<int>(200));
  for (int i = 0; i < 100 && fetches.load() < 2; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(fetches.load(), 2);
  EXPECT_EQ(sum.load(), 300);

  sched.Shutdown();
}

}  // namespace
