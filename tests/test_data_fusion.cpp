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
using minicyber::data::Notifier;
using minicyber::data::VisitorConfig;

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
  // M0 的 Fill 回调尝试 AllLatest，但 M1 为空，因此不产生融合结果。
  DataVisitor<int, int> dv(VisitorConfig{9203, 4}, VisitorConfig{9204, 4});
  DataDispatcher<int>::Instance()->Dispatch(9203, std::make_shared<int>(11));
  std::shared_ptr<int> m0, m1;
  EXPECT_FALSE(dv.TryFetch(m0, m1));
}

TEST(DataVisitorFusionTest, OnlySecondaryDispatchedReturnsFalse) {
  // M1 到达只更新最新值，不触发主通道 Fill 回调，也不填充融合队列。
  DataVisitor<int, int> dv(VisitorConfig{9205, 4}, VisitorConfig{9206, 4});
  DataDispatcher<int>::Instance()->Dispatch(9206, std::make_shared<int>(22));
  std::shared_ptr<int> m0, m1;
  EXPECT_FALSE(dv.TryFetch(m0, m1));
}

TEST(DataVisitorFusionTest, SecondaryThenPrimaryTriggersFusion) {
  // 先发送 M1，再发送 M0。M0 的 Fill 回调读取 M1 最新值并填充融合队列。
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

}  // namespace
