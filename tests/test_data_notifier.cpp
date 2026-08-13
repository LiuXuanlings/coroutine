#include "minicyber/data/data_notifier.h"

#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <thread>

namespace {

using minicyber::data::DataNotifier;
using minicyber::data::Notifier;

TEST(DataNotifierTest, NotifyOnUnknownChannelReturnsFalse) {
  auto* dn = DataNotifier::Instance();
  EXPECT_FALSE(dn->Notify(99999));
}

TEST(DataNotifierTest, AddNotifierThenNotifyInvokesCallback) {
  auto* dn = DataNotifier::Instance();
  auto n = std::make_shared<Notifier>();
  std::atomic<int> counter{0};
  n->SetCallback(
      [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
  dn->AddNotifier(1001, n);
  EXPECT_TRUE(dn->Notify(1001));
  EXPECT_EQ(counter.load(), 1);
}

TEST(DataNotifierTest, MultipleNotifiersOnSameChannelAllFire) {
  auto* dn = DataNotifier::Instance();
  std::atomic<int> counter{0};
  for (int i = 0; i < 3; ++i) {
    auto n = std::make_shared<Notifier>();
    n->SetCallback(
        [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
    dn->AddNotifier(2002, n);
  }
  EXPECT_TRUE(dn->Notify(2002));
  EXPECT_EQ(counter.load(), 3);
}

TEST(DataNotifierTest, EmptyCallbackIsSkippedSafely) {
  auto* dn = DataNotifier::Instance();
  auto n = std::make_shared<Notifier>();
  n->SetCallback(nullptr);  // empty
  dn->AddNotifier(3003, n);
  // Should not crash; Notify still returns true (channel exists).
  EXPECT_TRUE(dn->Notify(3003));
}

TEST(DataNotifierTest, CrossChannelIsolation) {
  auto* dn = DataNotifier::Instance();
  std::atomic<int> a_counter{0};
  std::atomic<int> b_counter{0};
  auto na = std::make_shared<Notifier>();
  na->SetCallback(
      [&a_counter]() { a_counter.fetch_add(1, std::memory_order_relaxed); });
  auto nb = std::make_shared<Notifier>();
  nb->SetCallback(
      [&b_counter]() { b_counter.fetch_add(1, std::memory_order_relaxed); });
  dn->AddNotifier(4001, na);
  dn->AddNotifier(4002, nb);
  EXPECT_TRUE(dn->Notify(4001));
  EXPECT_EQ(a_counter.load(), 1);
  EXPECT_EQ(b_counter.load(), 0);
  EXPECT_TRUE(dn->Notify(4002));
  EXPECT_EQ(a_counter.load(), 1);
  EXPECT_EQ(b_counter.load(), 1);
}

TEST(DataNotifierTest, RepeatedNotifyReInvokesCallbacks) {
  auto* dn = DataNotifier::Instance();
  auto n = std::make_shared<Notifier>();
  std::atomic<int> counter{0};
  n->SetCallback(
      [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
  dn->AddNotifier(5005, n);
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(dn->Notify(5005));
  }
  EXPECT_EQ(counter.load(), 5);
}

TEST(DataNotifierTest, ConcurrentAddAndNotifyIsSafe) {
  auto* dn = DataNotifier::Instance();
  constexpr int kThreads = 4;
  constexpr int kPerThread = 100;
  std::atomic<int> total{0};
  std::vector<std::thread> ths;
  for (int t = 0; t < kThreads; ++t) {
    ths.emplace_back([&]() {
      for (int i = 0; i < kPerThread; ++i) {
        auto n = std::make_shared<Notifier>();
        n->SetCallback(
            [&total]() { total.fetch_add(1, std::memory_order_relaxed); });
        dn->AddNotifier(6006, n);
      }
    });
  }
  for (auto& th : ths) th.join();
  EXPECT_TRUE(dn->Notify(6006));
  EXPECT_EQ(total.load(), kThreads * kPerThread);
}

TEST(DataNotifierTest, RemoveNotifierWaitsForInFlightCallback) {
  auto* dn = DataNotifier::Instance();
  constexpr uint64_t kChannel = 7007;
  auto notifier = std::make_shared<Notifier>();
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  notifier->SetCallback([&]() {
    entered.store(true, std::memory_order_release);
    while (!release.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  });
  dn->AddNotifier(kChannel, notifier);

  std::thread notifying([&]() { EXPECT_TRUE(dn->Notify(kChannel)); });
  while (!entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  std::atomic<bool> removed{false};
  std::thread removing([&]() {
    removed.store(dn->RemoveNotifier(kChannel, notifier),
                  std::memory_order_release);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  EXPECT_FALSE(removed.load(std::memory_order_acquire));
  release.store(true, std::memory_order_release);
  notifying.join();
  removing.join();
  EXPECT_TRUE(removed.load());
  EXPECT_FALSE(dn->Notify(kChannel));
}

TEST(DataNotifierTest, CallbackCanRemoveItselfWithoutDeadlock) {
  auto* dn = DataNotifier::Instance();
  constexpr uint64_t kChannel = 7008;
  auto notifier = std::make_shared<Notifier>();
  std::atomic<int> callbacks{0};
  notifier->SetCallback([&]() {
    callbacks.fetch_add(1, std::memory_order_relaxed);
    EXPECT_TRUE(dn->RemoveNotifier(kChannel, notifier));
  });
  dn->AddNotifier(kChannel, notifier);

  EXPECT_TRUE(dn->Notify(kChannel));
  EXPECT_EQ(callbacks.load(), 1);
  EXPECT_FALSE(dn->Notify(kChannel));
}

}  // namespace
