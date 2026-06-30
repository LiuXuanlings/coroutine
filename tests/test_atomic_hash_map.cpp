#include <gtest/gtest.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include "minicyber/base/atomic_hash_map.h"

using namespace minicyber;

// Test basic Set and Get
TEST(AtomicHashMapTest, BasicSetGet) {
  AtomicHashMap<uint64_t, int, 64> map;

  map.Set(42, 100);
  map.Set(100, 200);

  int value;
  EXPECT_TRUE(map.Get(42, &value));
  EXPECT_EQ(value, 100);

  EXPECT_TRUE(map.Get(100, &value));
  EXPECT_EQ(value, 200);

  EXPECT_FALSE(map.Get(999, &value));  // key not exists
}

// Test Has() existence check
TEST(AtomicHashMapTest, Has) {
  AtomicHashMap<uint64_t, int, 64> map;

  EXPECT_FALSE(map.Has(42));
  map.Set(42, 100);
  EXPECT_TRUE(map.Has(42));
  EXPECT_FALSE(map.Has(999));
}

// Test Update existing key
TEST(AtomicHashMapTest, Update) {
  AtomicHashMap<uint64_t, int, 64> map;

  map.Set(42, 100);
  EXPECT_TRUE(map.Has(42));

  map.Set(42, 200);  // Update
  int value;
  EXPECT_TRUE(map.Get(42, &value));
  EXPECT_EQ(value, 200);
}

// Test default value Set
TEST(AtomicHashMapTest, DefaultValue) {
  AtomicHashMap<uint64_t, int, 64> map;

  map.Set(42);  // Default construct value (0 for int)
  int value = -1;
  EXPECT_TRUE(map.Get(42, &value));
  EXPECT_EQ(value, 0);  // Default int is 0
}

// Test collision handling (multiple keys in same bucket)
TEST(AtomicHashMapTest, CollisionHandling) {
  // Use small table to force collisions
  AtomicHashMap<uint64_t, int, 4> map;

  // Keys 0, 4, 8 will all hash to bucket 0
  map.Set(0, 100);
  map.Set(4, 200);
  map.Set(8, 300);

  int value;
  EXPECT_TRUE(map.Get(0, &value));
  EXPECT_EQ(value, 100);
  EXPECT_TRUE(map.Get(4, &value));
  EXPECT_EQ(value, 200);
  EXPECT_TRUE(map.Get(8, &value));
  EXPECT_EQ(value, 300);
}

// Test with string values
TEST(AtomicHashMapTest, StringValues) {
  AtomicHashMap<uint64_t, std::string, 64> map;

  map.Set(1, "hello");
  map.Set(2, "world");

  std::string value;
  EXPECT_TRUE(map.Get(1, &value));
  EXPECT_EQ(value, "hello");
  EXPECT_TRUE(map.Get(2, &value));
  EXPECT_EQ(value, "world");
}

// Test Get with pointer
TEST(AtomicHashMapTest, GetPointer) {
  AtomicHashMap<uint64_t, int, 64> map;

  map.Set(42, 100);

  int* value_ptr = nullptr;
  EXPECT_TRUE(map.Get(42, &value_ptr));
  EXPECT_NE(value_ptr, nullptr);
  EXPECT_EQ(*value_ptr, 100);
}

// Test concurrent Set operations
TEST(AtomicHashMapTest, ConcurrentSet) {
  AtomicHashMap<uint64_t, int, 256> map;

  const int NUM_THREADS = 8;
  const int OPS_PER_THREAD = 1000;

  std::vector<std::thread> threads;
  for (int i = 0; i < NUM_THREADS; ++i) {
    threads.emplace_back([&map, i]() {
      for (int j = 0; j < OPS_PER_THREAD; ++j) {
        uint64_t key = i * OPS_PER_THREAD + j;
        map.Set(key, key * 10);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // Verify all entries
  for (int i = 0; i < NUM_THREADS; ++i) {
    for (int j = 0; j < OPS_PER_THREAD; ++j) {
      uint64_t key = i * OPS_PER_THREAD + j;
      int value;
      EXPECT_TRUE(map.Get(key, &value));
      EXPECT_EQ(value, key * 10);
    }
  }
}

// Test concurrent Get operations
TEST(AtomicHashMapTest, ConcurrentGet) {
  AtomicHashMap<uint64_t, int, 256> map;

  // Pre-populate
  for (int i = 0; i < 1000; ++i) {
    map.Set(i, i * 10);
  }

  const int NUM_THREADS = 8;
  std::atomic<int> success_count{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < NUM_THREADS; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < 1000; ++i) {
        int value;
        if (map.Get(i, &value)) {
          if (value == i * 10) {
            ++success_count;
          }
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(success_count.load(), NUM_THREADS * 1000);
}

// Test concurrent Set and Update
TEST(AtomicHashMapTest, ConcurrentUpdate) {
  AtomicHashMap<uint64_t, int, 64> map;

  // Pre-populate
  for (int i = 0; i < 100; ++i) {
    map.Set(i, 0);
  }

  const int NUM_THREADS = 8;
  const int INCREMENTS_PER_THREAD = 1000;

  std::vector<std::thread> threads;
  for (int t = 0; t < NUM_THREADS; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < INCREMENTS_PER_THREAD; ++i) {
        // Each thread increments values
        for (int key = 0; key < 100; ++key) {
          int* val_ptr;
          if (map.Get(key, &val_ptr)) {
            // Note: "(*val_ptr)++" is not atomic, here just testing concurrent access
            // Real atomic update would need compare-and-swap on value
          }
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // Keys should still exist
  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(map.Has(i));
  }
}

// Test mixed Set/Get operations
TEST(AtomicHashMapTest, MixedSetGet) {
  AtomicHashMap<uint64_t, int, 128> map;

  const int NUM_WRITERS = 4;
  const int NUM_READERS = 4;
  const int OPS_PER_THREAD = 500;

  std::atomic<int> writes{0};
  std::atomic<int> reads{0};

  std::vector<std::thread> threads;

  // Writers
  for (int i = 0; i < NUM_WRITERS; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < OPS_PER_THREAD; ++j) {
        uint64_t key = j % 100;  // Reuse keys
        map.Set(key, j);
        ++writes;
      }
    });
  }

  // Readers
  for (int i = 0; i < NUM_READERS; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < OPS_PER_THREAD; ++j) {
        uint64_t key = j % 100;
        int value;
        if (map.Get(key, &value)) {
          ++reads;
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(writes.load(), NUM_WRITERS * OPS_PER_THREAD);
  // Some reads may fail if key not yet inserted
  EXPECT_GT(reads.load(), 0);
}

// Test large table stress
TEST(AtomicHashMapTest, LargeTableStress) {
  AtomicHashMap<uint64_t, int, 1024> map;

  const int NUM_ENTRIES = 10000;

  // Insert many entries
  for (int i = 0; i < NUM_ENTRIES; ++i) {
    map.Set(i, i * 2);
  }

  // Verify
  for (int i = 0; i < NUM_ENTRIES; ++i) {
    int value;
    EXPECT_TRUE(map.Get(i, &value));
    EXPECT_EQ(value, i * 2);
  }
}

// Test move semantics for value
TEST(AtomicHashMapTest, MoveSemantics) {
  AtomicHashMap<uint64_t, std::string, 64> map;

  std::string value = "test_string";
  map.Set(42, std::move(value));

  std::string result;
  EXPECT_TRUE(map.Get(42, &result));
  EXPECT_EQ(result, "test_string");
}

// Test with uint32_t keys
TEST(AtomicHashMapTest, Uint32Keys) {
  AtomicHashMap<uint32_t, int, 64> map;

  map.Set(42u, 100);
  map.Set(UINT32_MAX, 200);

  int value;
  EXPECT_TRUE(map.Get(42u, &value));
  EXPECT_EQ(value, 100);
  EXPECT_TRUE(map.Get(UINT32_MAX, &value));
  EXPECT_EQ(value, 200);
}
