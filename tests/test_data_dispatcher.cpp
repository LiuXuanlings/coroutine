#include "minicyber/data/data_dispatcher.h"

#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <thread>

namespace {

using minicyber::data::CacheBuffer;
using minicyber::data::ChannelBuffer;
using minicyber::data::DataDispatcher;
using minicyber::data::DataNotifier;
using minicyber::data::Notifier;

TEST(DataDispatcherTest, DispatchOnUnknownChannelReturnsFalse) {
  auto* disp = DataDispatcher<int>::Instance();
  EXPECT_FALSE(disp->Dispatch(77777, std::make_shared<int>(-1)));
}

// Helper: register a no-op notifier so Dispatch's Notify returns true.
static void RegisterNoopNotifier(uint64_t channel_id) {
  auto* dn = DataNotifier::Instance();
  auto n = std::make_shared<Notifier>();
  n->callback = []() {};
  dn->AddNotifier(channel_id, n);
}

TEST(DataDispatcherTest, AddBufferThenDispatchFillsBuffer) {
  auto* disp = DataDispatcher<int>::Instance();
  RegisterNoopNotifier(8001);
  auto buf = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  ChannelBuffer<int> cb(8001, buf);
  disp->AddBuffer(cb);
  auto msg = std::make_shared<int>(42);
  EXPECT_TRUE(disp->Dispatch(8001, msg));
  std::shared_ptr<int> out;
  ASSERT_TRUE(cb.Latest(out));
  EXPECT_EQ(*out, 42);
}

TEST(DataDispatcherTest, MultipleBuffersOnSameChannelAllFilled) {
  auto* disp = DataDispatcher<int>::Instance();
  RegisterNoopNotifier(8002);
  auto bufA = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  auto bufB = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  ChannelBuffer<int> cbA(8002, bufA);
  ChannelBuffer<int> cbB(8002, bufB);
  disp->AddBuffer(cbA);
  disp->AddBuffer(cbB);
  auto msg = std::make_shared<int>(7);
  EXPECT_TRUE(disp->Dispatch(8002, msg));
  std::shared_ptr<int> outA, outB;
  ASSERT_TRUE(cbA.Latest(outA));
  ASSERT_TRUE(cbB.Latest(outB));
  EXPECT_EQ(*outA, 7);
  EXPECT_EQ(*outB, 7);
}

TEST(DataDispatcherTest, DeadWeakPtrIsSkippedWithoutCrash) {
  auto* disp = DataDispatcher<int>::Instance();
  RegisterNoopNotifier(8003);
  // Register a buffer in a scope, then let it die.
  {
    auto buf = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
    ChannelBuffer<int> cb(8003, buf);
    disp->AddBuffer(cb);
  }
  // Now that buffer's weak_ptr is dead. Register a live one on the same channel.
  auto live_buf = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  ChannelBuffer<int> live_cb(8003, live_buf);
  disp->AddBuffer(live_cb);
  auto msg = std::make_shared<int>(99);
  EXPECT_TRUE(disp->Dispatch(8003, msg));
  // Live buffer got the message; dead one was skipped.
  std::shared_ptr<int> out;
  ASSERT_TRUE(live_cb.Latest(out));
  EXPECT_EQ(*out, 99);
}

TEST(DataDispatcherTest, DispatchFiresDataNotifierCallback) {
  auto* disp = DataDispatcher<int>::Instance();
  auto* dn = DataNotifier::Instance();
  std::atomic<int> counter{0};
  auto n = std::make_shared<Notifier>();
  n->callback = [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); };
  dn->AddNotifier(8004, n);

  auto buf = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  ChannelBuffer<int> cb(8004, buf);
  disp->AddBuffer(cb);
  EXPECT_TRUE(disp->Dispatch(8004, std::make_shared<int>(1)));
  EXPECT_TRUE(disp->Dispatch(8004, std::make_shared<int>(2)));
  EXPECT_EQ(counter.load(), 2);
}

TEST(DataDispatcherTest, CrossChannelIsolation) {
  auto* disp = DataDispatcher<int>::Instance();
  auto bufA = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  auto bufB = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  ChannelBuffer<int> cbA(8005, bufA);
  ChannelBuffer<int> cbB(8006, bufB);
  disp->AddBuffer(cbA);
  disp->AddBuffer(cbB);
  disp->Dispatch(8005, std::make_shared<int>(111));
  disp->Dispatch(8006, std::make_shared<int>(222));
  std::shared_ptr<int> outA, outB;
  ASSERT_TRUE(cbA.Latest(outA));
  ASSERT_TRUE(cbB.Latest(outB));
  EXPECT_EQ(*outA, 111);
  EXPECT_EQ(*outB, 222);
}

TEST(DataDispatcherTest, ConcurrentDispatchOnSameChannelIsSafe) {
  auto* disp = DataDispatcher<int>::Instance();
  RegisterNoopNotifier(8007);
  auto buf = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(1000);
  ChannelBuffer<int> cb(8007, buf);
  disp->AddBuffer(cb);
  constexpr int kThreads = 4;
  constexpr int kPerThread = 200;
  std::vector<std::thread> ths;
  for (int t = 0; t < kThreads; ++t) {
    ths.emplace_back([&, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        disp->Dispatch(8007, std::make_shared<int>(t * kPerThread + i));
      }
    });
  }
  for (auto& th : ths) th.join();
  // 800 messages into a 1000-capacity buffer: not full, all retained.
  EXPECT_EQ(buf->Size(), static_cast<uint64_t>(kThreads * kPerThread));
  EXPECT_FALSE(buf->Full());
}

TEST(DataDispatcherTest, CallbackReentryDoesNotDeadlock) {
  // A Notify callback that itself calls Dispatch on a different channel
  // must not deadlock against the held map lock (we release it before Notify).
  auto* disp = DataDispatcher<int>::Instance();
  auto* dn = DataNotifier::Instance();

  // Channel 8008 callback dispatches onto channel 8009.
  auto n = std::make_shared<Notifier>();
  n->callback = [&]() {
    disp->Dispatch(8009, std::make_shared<int>(555));
  };
  dn->AddNotifier(8008, n);

  auto buf8 = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  auto buf9 = std::make_shared<CacheBuffer<std::shared_ptr<int>>>(4);
  ChannelBuffer<int> cb8(8008, buf8);
  ChannelBuffer<int> cb9(8009, buf9);
  disp->AddBuffer(cb8);
  disp->AddBuffer(cb9);

  EXPECT_TRUE(disp->Dispatch(8008, std::make_shared<int>(123)));
  std::shared_ptr<int> out8, out9;
  ASSERT_TRUE(cb8.Latest(out8));
  ASSERT_TRUE(cb9.Latest(out9));
  EXPECT_EQ(*out8, 123);
  EXPECT_EQ(*out9, 555);
}

}  // namespace