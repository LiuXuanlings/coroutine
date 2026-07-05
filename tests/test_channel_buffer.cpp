#include "minicyber/data/channel_buffer.h"

#include <gtest/gtest.h>
#include <memory>

namespace {

using minicyber::data::CacheBuffer;
using minicyber::data::ChannelBuffer;

TEST(ChannelBufferTest, ChannelIdIsPreserved) {
  auto buf = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  ChannelBuffer<int> cb(123, buf);
  EXPECT_EQ(cb.channel_id(), 123u);
  EXPECT_EQ(cb.Buffer().get(), buf.get());
}

TEST(ChannelBufferTest, LatestOnEmptyReturnsFalse) {
  auto buf = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  ChannelBuffer<int> cb(1, buf);
  std::shared_ptr<int> m = std::make_shared<int>(-1);
  EXPECT_FALSE(cb.Latest(m));
  EXPECT_EQ(*m, -1);  // -1 untouched on failure
}

TEST(ChannelBufferTest, LatestReturnsNewest) {
  auto buf = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  ChannelBuffer<int> cb(1, buf);
  buf->Fill(std::make_shared<int>(10));
  buf->Fill(std::make_shared<int>(20));
  buf->Fill(std::make_shared<int>(30));
  std::shared_ptr<int> m;
  ASSERT_TRUE(cb.Latest(m));
  EXPECT_EQ(*m, 30);
}

TEST(ChannelBufferTest, FetchWithZeroIndexReturnsLatestAndSetsIndex) {
  auto buf = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  ChannelBuffer<int> cb(1, buf);
  buf->Fill(std::make_shared<int>(10));
  buf->Fill(std::make_shared<int>(20));
  uint64_t index = 0;
  std::shared_ptr<int> m;
  ASSERT_TRUE(cb.Fetch(&index, m));
  EXPECT_EQ(*m, 20);
  EXPECT_EQ(index, buf->Tail());
}

TEST(ChannelBufferTest, FetchWhenAlreadyCaughtUpReturnsFalse) {
  auto buf = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  ChannelBuffer<int> cb(1, buf);
  buf->Fill(std::make_shared<int>(10));
  uint64_t index = 0;
  std::shared_ptr<int> m;
  ASSERT_TRUE(cb.Fetch(&index, m));  // index = Tail = 1
  EXPECT_EQ(*m, 10);
  // Fetch returns the element AT *index; the caller bumps index to request
  // the next one. index == Tail + 1 means caught up.
  index++;
  EXPECT_FALSE(cb.Fetch(&index, m));
}

TEST(ChannelBufferTest, FetchAdvancesAcrossNewFills) {
  auto buf = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  ChannelBuffer<int> cb(1, buf);
  buf->Fill(std::make_shared<int>(10));  // Tail = 1
  uint64_t index = 0;
  std::shared_ptr<int> m;
  ASSERT_TRUE(cb.Fetch(&index, m));  // index -> Tail = 1, returns element at 1
  EXPECT_EQ(*m, 10);
  buf->Fill(std::make_shared<int>(20));  // Tail = 2
  buf->Fill(std::make_shared<int>(30));  // Tail = 3
  index++;  // request next: 2
  ASSERT_TRUE(cb.Fetch(&index, m));
  EXPECT_EQ(*m, 20);
  index++;  // request next: 3
  ASSERT_TRUE(cb.Fetch(&index, m));
  EXPECT_EQ(*m, 30);
  index++;  // 4 == Tail + 1 -> caught up
  EXPECT_FALSE(cb.Fetch(&index, m));
}

TEST(ChannelBufferTest, FetchMultiReturnsOldestFirst) {
  auto buf = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  ChannelBuffer<int> cb(1, buf);
  for (int i = 1; i <= 4; ++i) buf->Fill(std::make_shared<int>(i));
  std::vector<std::shared_ptr<int>> vec;
  ASSERT_TRUE(cb.FetchMulti(3, &vec));
  EXPECT_EQ(vec.size(), 3u);
  EXPECT_EQ(*vec[0], 2);
  EXPECT_EQ(*vec[1], 3);
  EXPECT_EQ(*vec[2], 4);
}

TEST(ChannelBufferTest, FetchMultiClampsToSize) {
  auto buf = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  ChannelBuffer<int> cb(1, buf);
  buf->Fill(std::make_shared<int>(7));
  buf->Fill(std::make_shared<int>(8));
  std::vector<std::shared_ptr<int>> vec;
  ASSERT_TRUE(cb.FetchMulti(10, &vec));
  EXPECT_EQ(vec.size(), 2u);
  EXPECT_EQ(*vec[0], 7);
  EXPECT_EQ(*vec[1], 8);
}

TEST(ChannelBufferTest, FetchMultiOnEmptyReturnsFalse) {
  auto buf = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  ChannelBuffer<int> cb(1, buf);
  std::vector<std::shared_ptr<int>> vec;
  EXPECT_FALSE(cb.FetchMulti(3, &vec));
}

TEST(ChannelBufferTest, FetchOverflowFastForwardsToTail) {
  // capacity 3 -> internal vector size 4, live window = 3.
  auto buf = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(3);
  ChannelBuffer<int> cb(1, buf);
  for (int i = 1; i <= 3; ++i) buf->Fill(std::make_shared<int>(i));
  // Live window: [1,2,3], Head=1, Tail=3.
  uint64_t index = 0;
  std::shared_ptr<int> m;
  ASSERT_TRUE(cb.Fetch(&index, m));
  EXPECT_EQ(index, 3u);
  // Push two more -> overwrites 1 and 2. Live window: [3,4,5], Head=3, Tail=5.
  buf->Fill(std::make_shared<int>(4));
  buf->Fill(std::make_shared<int>(5));
  // 当前index=3和Head相等，不属于过期游标，无法触发过期快进逻辑；
  // 若用index=3反复读取，只会逐条读到末尾，最终走到无新数据分支。
  // 因此手动把index强制改为1（小于Head的过期下标），专门测试过期游标自动快进逻辑。
  index = 1;  // older than Head
  ASSERT_TRUE(cb.Fetch(&index, m));  // fast-forward to Tail
  EXPECT_EQ(index, 5u);
  EXPECT_EQ(*m, 5);
}

TEST(ChannelBufferTest, TwoChannelsShareBufferButDifferById) {
  auto buf = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  ChannelBuffer<int> cbA(11, buf);
  ChannelBuffer<int> cbB(22, buf);
  EXPECT_NE(cbA.channel_id(), cbB.channel_id());
  // Both back the same underlying buffer.
  EXPECT_EQ(cbA.Buffer().get(), cbB.Buffer().get());
  buf->Fill(std::make_shared<int>(99));
  std::shared_ptr<int> mA, mB;
  ASSERT_TRUE(cbA.Latest(mA));
  ASSERT_TRUE(cbB.Latest(mB));
  EXPECT_EQ(*mA, *mB);
}

}  // namespace