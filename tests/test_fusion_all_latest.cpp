// =============================================================================
// Step 34: AllLatest Fusion Strategy 单测
//
// 测试 AllLatest<M0, M1> 的 Fusion 行为：
//   1. 两个通道都为空时返回 false
//   2. 仅有 primary 数据时返回 false（secondary 无数据）
//   3. 仅有 secondary 数据时返回 false（primary 未触发通知）
//   4. secondary 先到达，primary 后到达：返回最新两个值的融合对
//   5. 多次 primary 触发 → 多次融合，Fusion 按 index 推进消费
//   6. secondary 在上一次融合后无更新，primary 再次触发 →
//      secondary 使用 stale latest
//
// 注意：AllLatest 通过 DataNotifier 挂接到 primary channel，不依赖
//   Scheduler 或 CRoutine。所有测试在纯 data 层完成。
// =============================================================================

#include <gtest/gtest.h>

#include <memory>

#include "minicyber/data/cache_buffer.h"
#include "minicyber/data/channel_buffer.h"
#include "minicyber/data/data_dispatcher.h"
#include "minicyber/data/data_notifier.h"
#include "minicyber/data/fusion/all_latest.h"

namespace {

using minicyber::data::CacheBuffer;
using minicyber::data::ChannelBuffer;
using minicyber::data::DataDispatcher;
using minicyber::data::DataNotifier;
using minicyber::data::Notifier;

// 辅助：为 channel 注册一个 no-op Notifier，使 DataDispatcher::Dispatch
// 的 Notify 返回 true（有订阅者）。
static void RegisterNoopNotifier(uint64_t channel_id) {
  auto n = std::make_shared<Notifier>();
  n->SetCallback([]() {});
  DataNotifier::Instance()->AddNotifier(channel_id, n);
}

// 辅助：创建一个已注册到 DataDispatcher 的 ChannelBuffer<int>
struct ChannelPair {
  std::shared_ptr<CacheBuffer<std::shared_ptr<int>>> cache;
  ChannelBuffer<int> channel;
};

static ChannelPair MakeChannel(uint64_t channel_id, uint32_t capacity = 4) {
  auto cache = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(capacity);
  ChannelBuffer<int> channel(channel_id, cache);
  DataDispatcher<int>::Instance()->AddBuffer(channel);
  RegisterNoopNotifier(channel_id);
  return {cache, channel};
}

}  // namespace

// =============================================================================
// 1. 两个通道都为空 → Fusion 返回 false
// =============================================================================
TEST(AllLatestTest, EmptyChannelsReturnFalse) {
  auto [c1, ch1] = MakeChannel(9201);
  auto [c2, ch2] = MakeChannel(9202);
  (void)c1; (void)c2;

  minicyber::data::fusion::AllLatest<int, int> al(ch1, ch2);
  uint64_t index = 0;
  std::shared_ptr<int> m0, m1;
  EXPECT_FALSE(al.Fusion(&index, m0, m1));
}

// =============================================================================
// 2. 仅有 primary 数据 → Fusion 返回 false（secondary 无数据）
// =============================================================================
TEST(AllLatestTest, OnlyPrimaryDispatchedReturnsFalse) {
  auto [c1, ch1] = MakeChannel(9203);
  auto [c2, ch2] = MakeChannel(9204);
  (void)c2;

  minicyber::data::fusion::AllLatest<int, int> al(ch1, ch2);

  // Dispatch primary only.
  DataDispatcher<int>::Instance()->Dispatch(9203, std::make_shared<int>(100));
  uint64_t index = 0;
  std::shared_ptr<int> m0, m1;
  // AllLatest's notifier fires, tries Latest(secondary) which fails →
  // fusion buffer stays empty → Fusion returns false.
  EXPECT_FALSE(al.Fusion(&index, m0, m1));
}

// =============================================================================
// 3. 仅有 secondary 数据 → Fusion 返回 false
//    （AllLatest 只注册 Notifier 在 primary channel，secondary 的 Notify
//      不会触发融合回调）
// =============================================================================
TEST(AllLatestTest, OnlySecondaryDispatchedReturnsFalse) {
  auto [c1, ch1] = MakeChannel(9205);
  auto [c2, ch2] = MakeChannel(9206);
  (void)c1;

  minicyber::data::fusion::AllLatest<int, int> al(ch1, ch2);

  // Dispatch secondary only.
  DataDispatcher<int>::Instance()->Dispatch(9206, std::make_shared<int>(200));
  uint64_t index = 0;
  std::shared_ptr<int> m0, m1;
  // Primary's Notifier not triggered → fusion callback never fires →
  // fusion buffer stays empty → Fusion returns false.
  EXPECT_FALSE(al.Fusion(&index, m0, m1));
}

// =============================================================================
// 4. secondary 先到达，primary 后到达 → 返回最新两个值的融合对
// =============================================================================
TEST(AllLatestTest, BothDispatchedSecondaryFirstReturnsLatestOfEach) {
  auto [c1, ch1] = MakeChannel(9207);
  auto [c2, ch2] = MakeChannel(9208);

  minicyber::data::fusion::AllLatest<int, int> al(ch1, ch2);

  // Secondary first: seed secondary channel before primary fires.
  DataDispatcher<int>::Instance()->Dispatch(9208, std::make_shared<int>(200));
  // Now primary fires — AllLatest notifier sees both channels ready.
  DataDispatcher<int>::Instance()->Dispatch(9207, std::make_shared<int>(100));

  // Both channels have data; AllLatest should return one fused pair (100, 200).
  uint64_t index = 0;
  std::shared_ptr<int> m0, m1;
  ASSERT_TRUE(al.Fusion(&index, m0, m1));
  EXPECT_EQ(*m0, 100);
  EXPECT_EQ(*m1, 200);
}

// =============================================================================
// 5. 多次 primary 触发 → 多次融合，Fusion 按 index 推进消费
// =============================================================================
TEST(AllLatestTest, MultiplePrimaryFiresAccumulateFusionEntries) {
  auto [c1, ch1] = MakeChannel(9209, 10);
  auto [c2, ch2] = MakeChannel(9210, 10);

  // Pre-seed secondary with value so every primary fire produces a fusion.
  DataDispatcher<int>::Instance()->Dispatch(9210, std::make_shared<int>(999));

  minicyber::data::fusion::AllLatest<int, int> al(ch1, ch2);

  // Dispatch primary 3 times.
  DataDispatcher<int>::Instance()->Dispatch(9209, std::make_shared<int>(10));
  DataDispatcher<int>::Instance()->Dispatch(9209, std::make_shared<int>(20));
  DataDispatcher<int>::Instance()->Dispatch(9209, std::make_shared<int>(30));

  uint64_t index = 0;
  std::shared_ptr<int> m0, m1;

  // First Fusion(0) returns the LATEST entry (ChannelBuffer::Fetch jumps to Tail).
  ASSERT_TRUE(al.Fusion(&index, m0, m1));
  EXPECT_EQ(*m0, 30);
  EXPECT_EQ(*m1, 999);

  // Caught up: only 3 entries in fusion buffer, index now = Tail+1.
  EXPECT_FALSE(al.Fusion(&index, m0, m1));

  // Verify all 3 entries exist by walking from Head (index=1) to Tail.
  index = 1;
  ASSERT_TRUE(al.Fusion(&index, m0, m1));
  EXPECT_EQ(*m0, 10);
  EXPECT_EQ(*m1, 999);

  ASSERT_TRUE(al.Fusion(&index, m0, m1));
  EXPECT_EQ(*m0, 20);
  EXPECT_EQ(*m1, 999);

  ASSERT_TRUE(al.Fusion(&index, m0, m1));
  EXPECT_EQ(*m0, 30);
  EXPECT_EQ(*m1, 999);

  // No more fusion entries.
  EXPECT_FALSE(al.Fusion(&index, m0, m1));
}

// =============================================================================
// 6. secondary stale latest reused 跨 primary 多次触发
// =============================================================================
TEST(AllLatestTest, StaleSecondaryReusedAcrossPrimaryFires) {
  auto [c1, ch1] = MakeChannel(9211, 10);
  auto [c2, ch2] = MakeChannel(9212, 10);

  // Seed secondary.
  DataDispatcher<int>::Instance()->Dispatch(9212, std::make_shared<int>(50));

  minicyber::data::fusion::AllLatest<int, int> al(ch1, ch2);

  // First primary fire → fusion (a=100, b=50).
  DataDispatcher<int>::Instance()->Dispatch(9211, std::make_shared<int>(100));
  uint64_t index = 0;
  std::shared_ptr<int> m0, m1;
  ASSERT_TRUE(al.Fusion(&index, m0, m1));
  EXPECT_EQ(*m0, 100);
  EXPECT_EQ(*m1, 50);

  // Second primary fire without updating secondary → fusion (a=200, b=50).
  DataDispatcher<int>::Instance()->Dispatch(9211, std::make_shared<int>(200));
  ASSERT_TRUE(al.Fusion(&index, m0, m1));
  EXPECT_EQ(*m0, 200);
  EXPECT_EQ(*m1, 50);

  // Update secondary then fire primary → fusion (a=300, b=70).
  DataDispatcher<int>::Instance()->Dispatch(9212, std::make_shared<int>(70));
  DataDispatcher<int>::Instance()->Dispatch(9211, std::make_shared<int>(300));
  ASSERT_TRUE(al.Fusion(&index, m0, m1));
  EXPECT_EQ(*m0, 300);
  EXPECT_EQ(*m1, 70);
}
