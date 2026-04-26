#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include "minicyber/base/atomic_rw_lock.h"
#include "minicyber/base/rw_lock_guard.h"

using namespace minicyber;

// Test basic construction
TEST(AtomicRWLockTest, Construction) {
  AtomicRWLock lock1;                    // default: write_first = true
  AtomicRWLock lock2(false);             // write_first = false
  (void)lock1;
  (void)lock2;
  SUCCEED();
}

// Test basic read lock/unlock using guards
TEST(AtomicRWLockTest, BasicReadLock) {
  AtomicRWLock lock;
  {
    ReadLockGuard<AtomicRWLock> guard(lock);
    // Lock held here
    SUCCEED();
  }
  // Lock released after scope
}

// Test basic write lock/unlock using guards
TEST(AtomicRWLockTest, BasicWriteLock) {
  AtomicRWLock lock;
  {
    WriteLockGuard<AtomicRWLock> guard(lock);
    // Lock held here
    SUCCEED();
  }
  // Lock released after scope
}

// Test multiple concurrent readers (should not block each other)
TEST(AtomicRWLockTest, MultipleReaders) {
  AtomicRWLock lock;
  std::atomic<int> active_readers{0};
  std::atomic<int> max_concurrent_readers{0};
  const int NUM_READERS = 10;

  std::vector<std::thread> threads;
  for (int i = 0; i < NUM_READERS; ++i) {
    threads.emplace_back([&]() {
      ReadLockGuard<AtomicRWLock> guard(lock);
      int current = ++active_readers;
      // Update max seen
      int prev_max = max_concurrent_readers.load();
      while (current > prev_max && !max_concurrent_readers.compare_exchange_weak(prev_max, current)) {}
      // Hold lock briefly
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      --active_readers;
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // Multiple readers should be able to hold lock simultaneously
  EXPECT_GT(max_concurrent_readers.load(), 1);
  EXPECT_EQ(active_readers.load(), 0);
}

// Test writer blocks other writers
TEST(AtomicRWLockTest, WriterExclusion) {
  AtomicRWLock lock;
  std::atomic<bool> writer_active{false};
  std::atomic<int> concurrent_writers{0};
  std::atomic<int> max_concurrent_writers{0};
  const int NUM_WRITERS = 5;

  std::vector<std::thread> threads;
  for (int i = 0; i < NUM_WRITERS; ++i) {
    threads.emplace_back([&]() {
      WriteLockGuard<AtomicRWLock> guard(lock);
      int current = ++concurrent_writers;
      // Update max seen
      int prev_max = max_concurrent_writers.load();
      while (current > prev_max && !max_concurrent_writers.compare_exchange_weak(prev_max, current)) {}
      EXPECT_FALSE(writer_active.exchange(true));
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      writer_active = false;
      --concurrent_writers;
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // Only one writer at a time
  EXPECT_EQ(max_concurrent_writers.load(), 1);
}

// Test writer blocks readers
TEST(AtomicRWLockTest, WriterBlocksReaders) {
  AtomicRWLock lock;
  std::atomic<bool> write_done{false};
  std::atomic<bool> reader_started{false};

  std::thread writer([&]() {
    WriteLockGuard<AtomicRWLock> guard(lock);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    write_done = true;
  });

  // Give writer time to acquire lock
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  std::thread reader([&]() {
    reader_started = true;
    ReadLockGuard<AtomicRWLock> guard(lock);
    // Reader should only proceed after writer releases
    EXPECT_TRUE(write_done.load());
  });

  writer.join();
  reader.join();
  EXPECT_TRUE(reader_started.load());
}

// Test readers block writer
TEST(AtomicRWLockTest, ReadersBlockWriter) {
  AtomicRWLock lock;
  std::atomic<int> active_readers{0};
  std::atomic<bool> writer_started{false};
  std::atomic<bool> write_done{false};

  // Start multiple readers
  std::vector<std::thread> readers;
  for (int i = 0; i < 3; ++i) {
    readers.emplace_back([&]() {
      ReadLockGuard<AtomicRWLock> guard(lock);
      ++active_readers;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      --active_readers;
    });
  }

  // Give readers time to start
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_GT(active_readers.load(), 0);

  std::thread writer([&]() {
    writer_started = true;
    WriteLockGuard<AtomicRWLock> guard(lock);
    // Writer only proceeds after all readers release
    EXPECT_EQ(active_readers.load(), 0);
    write_done = true;
  });

  writer.join();
  for (auto& t : readers) {
    t.join();
  }

  EXPECT_TRUE(write_done.load());
}

// Test write_first priority: new reader blocks if writer is waiting
TEST(AtomicRWLockTest, WriteFirstPriority) {
  AtomicRWLock lock(true);  // write_first = true
  std::atomic<bool> first_reader_active{false};
  std::atomic<bool> writer_waiting{false};
  std::atomic<bool> second_reader_got_lock{false};
  std::atomic<bool> writer_got_lock{false};

  // First reader holds lock
  std::thread first_reader([&]() {
    ReadLockGuard<AtomicRWLock> guard(lock);
    first_reader_active = true;
    // Wait until writer is waiting
    while (!writer_waiting.load()) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  });

  // Wait for first reader to acquire
  while (!first_reader_active.load()) {
    std::this_thread::yield();
  }

  // Writer wants lock
  std::thread writer([&]() {
    writer_waiting = true;
    WriteLockGuard<AtomicRWLock> guard(lock);
    writer_got_lock = true;
    // Writer should get lock before new reader
    EXPECT_FALSE(second_reader_got_lock.load());
  });

  // Give writer time to start waiting
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Second reader tries to get lock (should block because writer is waiting)
  std::thread second_reader([&]() {
    // In write_first mode, this should wait until writer completes
    ReadLockGuard<AtomicRWLock> guard(lock);
    second_reader_got_lock = true;
    // Should see writer already got the lock
    EXPECT_TRUE(writer_got_lock.load());
  });

  first_reader.join();
  writer.join();
  second_reader.join();

  EXPECT_TRUE(writer_got_lock.load());
  EXPECT_TRUE(second_reader_got_lock.load());
}

// Test RAII auto-unlock on exception
TEST(AtomicRWLockTest, RAIIUnlockOnException) {
  AtomicRWLock lock;
  std::atomic<bool> writer_blocked{false};

  try {
    WriteLockGuard<AtomicRWLock> guard(lock);
    throw std::runtime_error("test exception");
  } catch (const std::runtime_error&) {
    // Lock should be released
  }

  // Should be able to acquire lock again
  std::thread writer([&]() {
    WriteLockGuard<AtomicRWLock> guard(lock);
    writer_blocked = false;
  });

  // If lock wasn't released, this would block
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  writer.join();
  SUCCEED();
}

// Test read-write-read pattern
TEST(AtomicRWLockTest, ReadWriteReadPattern) {
  AtomicRWLock lock;
  int shared_data = 0;

  // Reader 1
  {
    ReadLockGuard<AtomicRWLock> guard(lock);
    EXPECT_EQ(shared_data, 0);
  }

  // Writer
  {
    WriteLockGuard<AtomicRWLock> guard(lock);
    shared_data = 42;
  }

  // Reader 2
  {
    ReadLockGuard<AtomicRWLock> guard(lock);
    EXPECT_EQ(shared_data, 42);
  }
}

// Test high contention stress test
TEST(AtomicRWLockTest, HighContention) {
  AtomicRWLock lock;
  std::atomic<int> shared_counter{0};
  std::atomic<int> read_sum{0};

  const int NUM_READERS = 8;
  const int NUM_WRITERS = 4;
  const int OPS_PER_THREAD = 1000;

  std::vector<std::thread> threads;

  // Readers
  for (int i = 0; i < NUM_READERS; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < OPS_PER_THREAD; ++j) {
        ReadLockGuard<AtomicRWLock> guard(lock);
        read_sum += shared_counter.load();
      }
    });
  }

  // Writers
  for (int i = 0; i < NUM_WRITERS; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < OPS_PER_THREAD; ++j) {
        WriteLockGuard<AtomicRWLock> guard(lock);
        ++shared_counter;
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(shared_counter.load(), NUM_WRITERS * OPS_PER_THREAD);
}
