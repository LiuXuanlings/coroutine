#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include "minicyber/base/bounded_queue.h"

using namespace minicyber;

// Test basic initialization
TEST(BoundedQueueTest, Initialization) {
    BoundedQueue<int> queue;
    EXPECT_TRUE(queue.Init(100));
    EXPECT_EQ(queue.Size(), 0);
    EXPECT_TRUE(queue.Empty());
}

// Test basic enqueue and dequeue (single thread)
TEST(BoundedQueueTest, BasicEnqueueDequeue) {
    BoundedQueue<int> queue;
    ASSERT_TRUE(queue.Init(10));

    // Enqueue some elements
    EXPECT_TRUE(queue.Enqueue(42));
    EXPECT_TRUE(queue.Enqueue(43));
    EXPECT_TRUE(queue.Enqueue(44));
    EXPECT_EQ(queue.Size(), 3);

    // Dequeue and verify FIFO order
    int value;
    EXPECT_TRUE(queue.Dequeue(&value));
    EXPECT_EQ(value, 42);
    EXPECT_TRUE(queue.Dequeue(&value));
    EXPECT_EQ(value, 43);
    EXPECT_TRUE(queue.Dequeue(&value));
    EXPECT_EQ(value, 44);
    EXPECT_EQ(queue.Size(), 0);
    EXPECT_TRUE(queue.Empty());
}

// Test FIFO ordering with more elements
TEST(BoundedQueueTest, FifoOrder) {
    BoundedQueue<int> queue;
    ASSERT_TRUE(queue.Init(100));

    // Enqueue 0..99
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(queue.Enqueue(i));
    }

    // Dequeue and verify order
    int value;
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(queue.Dequeue(&value));
        EXPECT_EQ(value, i);
    }
}

// Test capacity boundary - queue full
TEST(BoundedQueueTest, QueueFull) {
    BoundedQueue<int> queue;
    ASSERT_TRUE(queue.Init(5));  // capacity = 5

    // Fill to capacity
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(queue.Enqueue(i));
    }

    // Queue should be full now
    EXPECT_FALSE(queue.Enqueue(99));
    EXPECT_EQ(queue.Size(), 5);

    // Dequeue one
    int value;
    EXPECT_TRUE(queue.Dequeue(&value));
    EXPECT_EQ(value, 0);

    // Now we can enqueue again
    EXPECT_TRUE(queue.Enqueue(99));
}

// Test capacity boundary - queue empty
TEST(BoundedQueueTest, QueueEmpty) {
    BoundedQueue<int> queue;
    ASSERT_TRUE(queue.Init(10));

    int value = 999;
    EXPECT_TRUE(queue.Empty());
    EXPECT_FALSE(queue.Dequeue(&value));
    EXPECT_EQ(value, 999);  // value should not be modified
}

// Test wrap-around behavior (ring buffer)
TEST(BoundedQueueTest, WrapAround) {
    BoundedQueue<int> queue;
    ASSERT_TRUE(queue.Init(5));

    // Fill
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(queue.Enqueue(i));
    }

    // Dequeue 3
    int value;
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(queue.Dequeue(&value));
        EXPECT_EQ(value, i);
    }

    // Enqueue 3 more (wraps around)
    EXPECT_TRUE(queue.Enqueue(100));
    EXPECT_TRUE(queue.Enqueue(101));
    EXPECT_TRUE(queue.Enqueue(102));

    // Dequeue remaining
    EXPECT_TRUE(queue.Dequeue(&value));
    EXPECT_EQ(value, 3);
    EXPECT_TRUE(queue.Dequeue(&value));
    EXPECT_EQ(value, 4);
    EXPECT_TRUE(queue.Dequeue(&value));
    EXPECT_EQ(value, 100);
    EXPECT_TRUE(queue.Dequeue(&value));
    EXPECT_EQ(value, 101);
    EXPECT_TRUE(queue.Dequeue(&value));
    EXPECT_EQ(value, 102);

    EXPECT_TRUE(queue.Empty());
}

// Test multi-threaded single producer single consumer (1P1C)
TEST(BoundedQueueTest, SingleProducerSingleConsumer) {
    BoundedQueue<int> queue;
    ASSERT_TRUE(queue.Init(100));

    const int NUM_ITEMS = 1000000;  // 1M items

    std::thread producer([&]() {
        for (int i = 0; i < NUM_ITEMS; ++i) {
            while (!queue.Enqueue(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&]() {
        int value;
        for (int i = 0; i < NUM_ITEMS; ++i) {
            while (!queue.Dequeue(&value)) {
                std::this_thread::yield();
            }
            EXPECT_EQ(value, i);
        }
    });

    producer.join();
    consumer.join();

    EXPECT_TRUE(queue.Empty());
}

// Test multi-threaded multi producer multi consumer (MPMC)
TEST(BoundedQueueTest, MultiProducerMultiConsumer) {
    BoundedQueue<int> queue;
    ASSERT_TRUE(queue.Init(1000));

    const int NUM_PRODUCERS = 4;
    const int NUM_CONSUMERS = 4;
    const int ITEMS_PER_PRODUCER = 100000;
    const int TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;

    std::atomic<int> total_consumed{0};
    std::atomic<int> enqueue_count{0};
    std::atomic<int> dequeue_count{0};

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    // Launch producers
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&]() {
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                while (!queue.Enqueue(i)) {
                    std::this_thread::yield();
                }
                ++enqueue_count;
            }
        });
    }

    // Launch consumers
    for (int c = 0; c < NUM_CONSUMERS; ++c) {
        consumers.emplace_back([&]() {
            int value;
            while (dequeue_count.load() < TOTAL_ITEMS) {
                if (queue.Dequeue(&value)) {
                    ++dequeue_count;
                    total_consumed += value;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }
    for (auto& t : consumers) {
        t.join();
    }

    EXPECT_EQ(enqueue_count.load(), TOTAL_ITEMS);
    EXPECT_EQ(dequeue_count.load(), TOTAL_ITEMS);
}

// Test WaitEnqueue and WaitDequeue with BlockWaitStrategy
TEST(BoundedQueueTest, WaitEnqueueDequeue) {
    BoundedQueue<int> queue;
    ASSERT_TRUE(queue.Init(5, new BlockWaitStrategy()));

    std::atomic<int> consumed{0};

    std::thread producer([&]() {
        for (int i = 0; i < 10; ++i) {
            EXPECT_TRUE(queue.WaitEnqueue(i));
        }
    });

    std::thread consumer([&]() {
        int value;
        for (int i = 0; i < 10; ++i) {
            EXPECT_TRUE(queue.WaitDequeue(&value));
            EXPECT_EQ(value, i);
            ++consumed;
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(consumed.load(), 10);
    EXPECT_TRUE(queue.Empty());
}

// Test BreakAllWait
TEST(BoundedQueueTest, BreakAllWait) {
    BoundedQueue<int> queue;
    ASSERT_TRUE(queue.Init(5, new BlockWaitStrategy()));

    std::atomic<bool> woken{false};

    std::thread waiter([&]() {
        int value;
        // This will block until BreakAllWait is called
        bool result = queue.WaitDequeue(&value);
        woken = !result;  // Should return false due to break
    });

    // Give waiter time to start waiting
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    queue.BreakAllWait();

    waiter.join();
    EXPECT_TRUE(woken.load());
}

// Test with string type (non-trivial type)
TEST(BoundedQueueTest, StringQueue) {
    BoundedQueue<std::string> queue;
    ASSERT_TRUE(queue.Init(10));

    EXPECT_TRUE(queue.Enqueue("hello"));
    EXPECT_TRUE(queue.Enqueue("world"));

    std::string value;
    EXPECT_TRUE(queue.Dequeue(&value));
    EXPECT_EQ(value, "hello");
    EXPECT_TRUE(queue.Dequeue(&value));
    EXPECT_EQ(value, "world");
}

// Note: BoundedQueue requires copyable types for Dequeue.
// Move-only types like unique_ptr are not directly supported by the reference design.
// Use shared_ptr or custom wrapper for such cases.

// Test Head/Tail/Commit accessors
TEST(BoundedQueueTest, AccessorMethods) {
    BoundedQueue<int> queue;
    ASSERT_TRUE(queue.Init(10));

    EXPECT_EQ(queue.Head(), 0);
    EXPECT_EQ(queue.Tail(), 1);
    EXPECT_EQ(queue.Commit(), 1);

    queue.Enqueue(1);
    EXPECT_EQ(queue.Tail(), 2);
    EXPECT_EQ(queue.Commit(), 2);

    queue.Enqueue(2);
    EXPECT_EQ(queue.Tail(), 3);
    EXPECT_EQ(queue.Commit(), 3);

    int value;
    queue.Dequeue(&value);
    EXPECT_EQ(queue.Head(), 1);
}

// Stress test - high contention
TEST(BoundedQueueTest, HighContention) {
    BoundedQueue<int> queue;
    ASSERT_TRUE(queue.Init(10));  // Small buffer = high contention

    const int NUM_THREADS = 8;
    const int OPS_PER_THREAD = 10000;

    std::vector<std::thread> threads;
    std::atomic<int> total_enqueued{0};
    std::atomic<int> total_dequeued{0};

    // Mixed producers and consumers
    for (int i = 0; i < NUM_THREADS; ++i) {
        if (i % 2 == 0) {
            threads.emplace_back([&]() {
                for (int j = 0; j < OPS_PER_THREAD; ++j) {
                    while (!queue.Enqueue(j)) {
                        std::this_thread::yield();
                    }
                    ++total_enqueued;
                }
            });
        } else {
            threads.emplace_back([&]() {
                int value;
                for (int j = 0; j < OPS_PER_THREAD; ++j) {
                    while (!queue.Dequeue(&value)) {
                        std::this_thread::yield();
                    }
                    ++total_dequeued;
                }
            });
        }
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(total_enqueued.load(), total_dequeued.load());
}
