#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include "minicyber/base/wait_strategy.h"

using namespace minicyber;

// Test YieldWaitStrategy returns immediately (very fast)
TEST(WaitStrategyTest, YieldWaitStrategy) {
    YieldWaitStrategy strategy;
    auto start = std::chrono::steady_clock::now();

    bool result = strategy.EmptyWait();

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    EXPECT_TRUE(result);
    EXPECT_LT(duration.count(), 1000);  // Should be much less than 1ms
}

// Test SleepWaitStrategy sleeps for expected duration
TEST(WaitStrategyTest, SleepWaitStrategy) {
    SleepWaitStrategy strategy(5000);  // 5ms
    auto start = std::chrono::steady_clock::now();

    bool result = strategy.EmptyWait();

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    EXPECT_TRUE(result);
    EXPECT_GE(duration.count(), 4);  // At least ~4ms (allow small variance)
    EXPECT_LT(duration.count(), 20); // But not too long
}

// Test SleepWaitStrategy setters work
TEST(WaitStrategyTest, SleepWaitStrategySetTimeout) {
    SleepWaitStrategy strategy;
    strategy.SetSleepTimeMicroSeconds(1000);  // 1ms

    auto start = std::chrono::steady_clock::now();
    strategy.EmptyWait();
    auto end = std::chrono::steady_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    EXPECT_LT(duration.count(), 10);  // Should be quick
}

// Test BlockWaitStrategy NotifyOne wakes one waiter
TEST(WaitStrategyTest, BlockWaitStrategyNotifyOne) {
    BlockWaitStrategy strategy;
    std::atomic<bool> woken{false};

    std::thread waiter([&]() {
        woken = strategy.EmptyWait();
    });

    // Give waiter time to start waiting
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    strategy.NotifyOne();

    waiter.join();
    EXPECT_TRUE(woken);
}

// Test BlockWaitStrategy BreakAllWait wakes all waiters
TEST(WaitStrategyTest, BlockWaitStrategyBreakAllWait) {
    BlockWaitStrategy strategy;
    std::atomic<int> woken_count{0};
    const int num_waiters = 5;
    std::vector<std::thread> waiters;

    for (int i = 0; i < num_waiters; ++i) {
        waiters.emplace_back([&]() {
            if (strategy.EmptyWait()) {
                ++woken_count;
            }
        });
    }

    // Give waiters time to start waiting
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    strategy.BreakAllWait();

    for (auto& t : waiters) {
        t.join();
    }

    EXPECT_EQ(woken_count, num_waiters);
}

// Test BusySpinWaitStrategy returns immediately
TEST(WaitStrategyTest, BusySpinWaitStrategy) {
    BusySpinWaitStrategy strategy;
    auto start = std::chrono::steady_clock::now();

    bool result = strategy.EmptyWait();

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    EXPECT_TRUE(result);
    EXPECT_LT(duration.count(), 100);  // Very fast
}

// Test TimeoutBlockWaitStrategy returns false on timeout
TEST(WaitStrategyTest, TimeoutBlockWaitStrategyTimeout) {
    TimeoutBlockWaitStrategy strategy(50);  // 50ms timeout

    auto start = std::chrono::steady_clock::now();
    bool result = strategy.EmptyWait();
    auto end = std::chrono::steady_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    EXPECT_FALSE(result);  // Should timeout
    EXPECT_GE(duration.count(), 40);  // At least ~40ms
}

// Test TimeoutBlockWaitStrategy returns true when notified
TEST(WaitStrategyTest, TimeoutBlockWaitStrategyNotify) {
    TimeoutBlockWaitStrategy strategy(5000);  // 5s timeout
    std::atomic<bool> woken{false};

    std::thread waiter([&]() {
        woken = strategy.EmptyWait();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    strategy.NotifyOne();

    waiter.join();
    EXPECT_TRUE(woken);
}

// Test TimeoutBlockWaitStrategy BreakAllWait wakes all waiters
TEST(WaitStrategyTest, TimeoutBlockWaitStrategyBreakAllWait) {
    TimeoutBlockWaitStrategy strategy(10000);  // 10s timeout
    std::atomic<int> woken_count{0};
    const int num_waiters = 3;
    std::vector<std::thread> waiters;

    for (int i = 0; i < num_waiters; ++i) {
        waiters.emplace_back([&]() {
            if (strategy.EmptyWait()) {
                ++woken_count;
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    strategy.BreakAllWait();

    for (auto& t : waiters) {
        t.join();
    }

    EXPECT_EQ(woken_count, num_waiters);
}

// Test TimeoutBlockWaitStrategy setters work
TEST(WaitStrategyTest, TimeoutBlockWaitStrategySetTimeout) {
    TimeoutBlockWaitStrategy strategy;
    strategy.SetTimeout(50);  // 50ms

    auto start = std::chrono::steady_clock::now();
    bool result = strategy.EmptyWait();
    auto end = std::chrono::steady_clock::now();

    EXPECT_FALSE(result);  // Should timeout
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    EXPECT_GE(duration.count(), 40);
}
