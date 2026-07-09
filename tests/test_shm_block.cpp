#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include "minicyber/transport/shm/block.h"

using minicyber::transport::Block;

// 测试夹具：暴露 Block 的私有锁方法
// 放在 minicyber::transport 命名空间内，以匹配 Block 中的 friend 声明
namespace minicyber::transport {
class ShmBlockTest {
 public:
  static bool TryLockForWrite(Block& b) { return b.TryLockForWrite(); }
  static bool TryLockForRead(Block& b) { return b.TryLockForRead(); }
  static void ReleaseWriteLock(Block& b) { b.ReleaseWriteLock(); }
  static void ReleaseReadLock(Block& b) { b.ReleaseReadLock(); }
  static int32_t LockNum(const Block& b) { return b.lock_num_.load(); }
};
}  // namespace minicyber::transport
using minicyber::transport::ShmBlockTest;

// 构造时 msg_size / msg_info_size 为 0
TEST(ShmBlockTest, InitZero) {
  Block b;
  EXPECT_EQ(b.msg_size(), 0u);
  EXPECT_EQ(b.msg_info_size(), 0u);
  EXPECT_EQ(ShmBlockTest::LockNum(b), 0);
}

// 常量值与 CyberRT 一致
TEST(ShmBlockTest, ConstantsMatchReference) {
  EXPECT_EQ(Block::kRWLockFree, 0);
  EXPECT_EQ(Block::kWriteExclusive, -1);
  EXPECT_EQ(Block::kMaxTryLockTimes, 5);
}

// setter/getter
TEST(ShmBlockTest, SetGetMsgSize) {
  Block b;
  b.set_msg_size(2048);
  b.set_msg_info_size(64);
  EXPECT_EQ(b.msg_size(), 2048u);
  EXPECT_EQ(b.msg_info_size(), 64u);
}

// 写锁：空闲 -> -1
TEST(ShmBlockTest, TryLockForWriteOnFree) {
  Block b;
  ASSERT_TRUE(ShmBlockTest::TryLockForWrite(b));
  EXPECT_EQ(ShmBlockTest::LockNum(b), -1);
}

// 写锁已被持有，再次 TryLockForWrite 失败
TEST(ShmBlockTest, TryLockForWriteWhenHeld) {
  Block b;
  ASSERT_TRUE(ShmBlockTest::TryLockForWrite(b));
  EXPECT_FALSE(ShmBlockTest::TryLockForWrite(b));
  EXPECT_FALSE(ShmBlockTest::TryLockForRead(b));
}

// 释放写锁后恢复空闲
TEST(ShmBlockTest, ReleaseWriteLock) {
  Block b;
  ASSERT_TRUE(ShmBlockTest::TryLockForWrite(b));
  ShmBlockTest::ReleaseWriteLock(b);
  EXPECT_EQ(ShmBlockTest::LockNum(b), 0);
  // 释放后可再次写锁
  EXPECT_TRUE(ShmBlockTest::TryLockForWrite(b));
}

// 读锁：空闲 -> 1
TEST(ShmBlockTest, TryLockForReadOnFree) {
  Block b;
  ASSERT_TRUE(ShmBlockTest::TryLockForRead(b));
  EXPECT_EQ(ShmBlockTest::LockNum(b), 1);
}

// 多个读锁可共存
TEST(ShmBlockTest, MultipleReadLocks) {
  Block b;
  ASSERT_TRUE(ShmBlockTest::TryLockForRead(b));
  ASSERT_TRUE(ShmBlockTest::TryLockForRead(b));
  ASSERT_TRUE(ShmBlockTest::TryLockForRead(b));
  EXPECT_EQ(ShmBlockTest::LockNum(b), 3);
}

// 写锁持有期间，读锁失败
TEST(ShmBlockTest, ReadLockBlockedByWrite) {
  Block b;
  ASSERT_TRUE(ShmBlockTest::TryLockForWrite(b));
  EXPECT_FALSE(ShmBlockTest::TryLockForRead(b));
}

// 读锁持有期间，写锁失败
TEST(ShmBlockTest, WriteLockBlockedByRead) {
  Block b;
  ASSERT_TRUE(ShmBlockTest::TryLockForRead(b));
  EXPECT_FALSE(ShmBlockTest::TryLockForWrite(b));
}

// 释放读锁：计数 -1
TEST(ShmBlockTest, ReleaseReadLock) {
  Block b;
  ASSERT_TRUE(ShmBlockTest::TryLockForRead(b));
  ASSERT_TRUE(ShmBlockTest::TryLockForRead(b));
  ShmBlockTest::ReleaseReadLock(b);
  EXPECT_EQ(ShmBlockTest::LockNum(b), 1);
  ShmBlockTest::ReleaseReadLock(b);
  EXPECT_EQ(ShmBlockTest::LockNum(b), 0);
}

// 读->写->读 完整流转
TEST(ShmBlockTest, ReadWriteReadFlow) {
  Block b;
  ASSERT_TRUE(ShmBlockTest::TryLockForRead(b));
  ShmBlockTest::ReleaseReadLock(b);
  ASSERT_TRUE(ShmBlockTest::TryLockForWrite(b));
  ShmBlockTest::ReleaseWriteLock(b);
  ASSERT_TRUE(ShmBlockTest::TryLockForRead(b));
  ShmBlockTest::ReleaseReadLock(b);
  EXPECT_EQ(ShmBlockTest::LockNum(b), 0);
}

// 多线程并发写锁互斥：同一时刻只有一个成功
TEST(ShmBlockTest, ConcurrentWriteExclusion) {
  Block b;
  std::atomic<int> success_count{0};
  std::atomic<int> fail_count{0};
  const int NUM_THREADS = 16;
  std::vector<std::thread> threads;
  for (int i = 0; i < NUM_THREADS; ++i) {
    threads.emplace_back([&]() {
      if (ShmBlockTest::TryLockForWrite(b)) {
        ++success_count;
        // 持有一会儿，放大竞争窗口
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        ShmBlockTest::ReleaseWriteLock(b);
      } else {
        ++fail_count;
      }
    });
  }
  for (auto& t : threads) t.join();
  // 至少有一个成功，且同时刻没有两个都成功（这里只验证总数 >= 1）
  EXPECT_GE(success_count.load(), 1);
  EXPECT_EQ(success_count.load() + fail_count.load(), NUM_THREADS);
  // 最终锁回到空闲
  EXPECT_EQ(ShmBlockTest::LockNum(b), 0);
}

// 多线程并发读锁：计数等于成功次数
TEST(ShmBlockTest, ConcurrentReadShared) {
  Block b;
  std::atomic<int> success_count{0};
  const int NUM_THREADS = 16;
  std::vector<std::thread> threads;
  for (int i = 0; i < NUM_THREADS; ++i) {
    threads.emplace_back([&]() {
      if (ShmBlockTest::TryLockForRead(b)) {
        ++success_count;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        ShmBlockTest::ReleaseReadLock(b);
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(success_count.load(), NUM_THREADS);
  EXPECT_EQ(ShmBlockTest::LockNum(b), 0);
}