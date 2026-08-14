#include "minicyber/data/data_visitor.h"

#include <gtest/gtest.h>
#include <atomic>
#include <memory>

#include "minicyber/data/data_dispatcher.h"
#include "minicyber/data/data_notifier.h"

namespace {

using minicyber::data::DataDispatcher;
using minicyber::data::DataNotifier;
using minicyber::data::DataVisitor;
using minicyber::data::VisitorConfig;

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

}  // namespace
