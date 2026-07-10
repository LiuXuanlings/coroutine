#include <gtest/gtest.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "minicyber/transport/shm/condition_notifier.h"

using minicyber::transport::ConditionNotifier;
using minicyber::transport::ReadableInfo;

namespace {
// 清理可能残留的同 key SHM，避免用例间相互干扰
void CleanupShm(key_t key) {
  int shmid = ::shmget(key, 0, 0644);
  if (shmid != -1) ::shmctl(shmid, IPC_RMID, 0);
}
}  // namespace

// Init 后 indicator 非空
TEST(ConditionNotifierTest, InitSucceeds) {
  ConditionNotifier n;
  CleanupShm(n.key());
  ASSERT_TRUE(n.Init());
  EXPECT_FALSE(n.IsShutdown());
  n.Shutdown();
  CleanupShm(n.key());
}

// Fd() 始终返回 -1（本方案无 epoll 桥）
TEST(ConditionNotifierTest, FdIsMinusOne) {
  ConditionNotifier n;
  CleanupShm(n.key());
  ASSERT_TRUE(n.Init());
  EXPECT_EQ(n.Fd(), -1);
  EXPECT_EQ(n.EpollFd(), -1);
  n.Shutdown();
  CleanupShm(n.key());
}

// Notify 后 Listen 立即返回 true 且内容一致
TEST(ConditionNotifierTest, NotifyThenListen) {
  ConditionNotifier n;
  CleanupShm(n.key());
  ASSERT_TRUE(n.Init());
  ReadableInfo sent{1234, 7, 42};
  ASSERT_TRUE(n.Notify(sent));
  ReadableInfo got;
  ASSERT_TRUE(n.Listen(100, &got));
  EXPECT_EQ(got.host_id, 1234u);
  EXPECT_EQ(got.block_index, 7u);
  EXPECT_EQ(got.channel_id, 42u);
  n.Shutdown();
  CleanupShm(n.key());
}

// 无通知时 Listen(0) 立即返回 false
TEST(ConditionNotifierTest, ListenNonBlockingNoNotify) {
  ConditionNotifier n;
  CleanupShm(n.key());
  ASSERT_TRUE(n.Init());
  ReadableInfo got;
  auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(n.Listen(0, &got));
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 50);
  n.Shutdown();
  CleanupShm(n.key());
}

// 无通知时 Listen(100) 最终返回 false。
// 注意：50µs 粒度 sleep 受 OS 调度器影响（最小调度粒度常 ~1ms），
// 实际耗时可能数倍于 100ms，这里只验证"超时返回 false"语义与下界。
TEST(ConditionNotifierTest, ListenTimeout) {
  ConditionNotifier n;
  CleanupShm(n.key());
  ASSERT_TRUE(n.Init());
  ReadableInfo got;
  auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(n.Listen(100, &got));
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count();
  // 下界保证确实等了；上界放宽到 5s 容忍调度器粒度
  EXPECT_GE(elapsed, 80);
  EXPECT_LE(elapsed, 5000);
  n.Shutdown();
  CleanupShm(n.key());
}

// 多次 Notify 顺序读出（FIFO）
TEST(ConditionNotifierTest, MultipleNotifyInOrder) {
  ConditionNotifier n;
  CleanupShm(n.key());
  ASSERT_TRUE(n.Init());
  for (uint32_t i = 0; i < 5; ++i) {
    ASSERT_TRUE(n.Notify({1, i, 99}));
  }
  ReadableInfo got;
  for (uint32_t i = 0; i < 5; ++i) {
    ASSERT_TRUE(n.Listen(100, &got));
    EXPECT_EQ(got.block_index, i);
  }
  n.Shutdown();
  CleanupShm(n.key());
}

// 超过 kBufLength 条后，最早的若干条被覆盖；
// Listen 的 fast-forward 语义：next_seq 落后时跳到槽位实际 seq，
// 因此读出的是"当前槽位里还存活"的那批，按 seq 顺序。
TEST(ConditionNotifierTest, RingWraparound) {
  ConditionNotifier n;
  CleanupShm(n.key());
  ASSERT_TRUE(n.Init());
  // 写 kBufLength + 10 条，最早的 10 条已被覆盖
  for (uint32_t i = 0; i < minicyber::transport::kBufLength + 10; ++i) {
    ASSERT_TRUE(n.Notify({0, i, 0}));
  }
  // 槽 0 现在存的是 seq=kBufLength（block_index=kBufLength）
  // 第一次 Listen 会 fast-forward 到 seq=kBufLength，读出 block_index=kBufLength
  // 之后顺序读到 seq=kBufLength+9（block_index=kBufLength+9）
  // 共 10 条
  ReadableInfo got;
  for (uint32_t k = 0; k < 10; ++k) {
    uint32_t expected_bi = minicyber::transport::kBufLength + k;
    ASSERT_TRUE(n.Listen(100, &got)) << "k=" << k;
    EXPECT_EQ(got.block_index, expected_bi) << "k=" << k;
  }
  // 再 Listen 应超时（已追上 next_seq）
  EXPECT_FALSE(n.Listen(50, &got));
  n.Shutdown();
  CleanupShm(n.key());
}

// Shutdown 后 Notify/Listen 失败
TEST(ConditionNotifierTest, ShutdownDisablesOps) {
  ConditionNotifier n;
  CleanupShm(n.key());
  ASSERT_TRUE(n.Init());
  n.Shutdown();
  EXPECT_TRUE(n.IsShutdown());
  EXPECT_FALSE(n.Notify({0, 0, 0}));
  ReadableInfo got;
  EXPECT_FALSE(n.Listen(100, &got));
  CleanupShm(n.key());
}

// Shutdown 幂等
TEST(ConditionNotifierTest, ShutdownIdempotent) {
  ConditionNotifier n;
  CleanupShm(n.key());
  ASSERT_TRUE(n.Init());
  n.Shutdown();
  n.Shutdown();
  n.Shutdown();
  SUCCEED();
  CleanupShm(n.key());
}

// 同 key 两个 ConditionNotifier：第二个走 OpenOnly，共享同一段 SHM
TEST(ConditionNotifierTest, TwoNotifiersSameKey) {
  CleanupShm(0);  // 预清理
  ConditionNotifier n1;
  CleanupShm(n1.key());
  ASSERT_TRUE(n1.Init());

  ConditionNotifier n2;
  ASSERT_TRUE(n2.Init());
  // n1 Notify，n2 Listen 应能读到
  ASSERT_TRUE(n1.Notify({5678, 3, 1}));
  ReadableInfo got;
  ASSERT_TRUE(n2.Listen(100, &got));
  EXPECT_EQ(got.host_id, 5678u);

  n1.Shutdown();
  n2.Shutdown();
  CleanupShm(n1.key());
}

// fork：父 Notify，子 Listen 醒来（验证跨进程共享同一段 SHM）
TEST(ConditionNotifierTest, ForkCrossProcessNotify) {
  ConditionNotifier n;
  CleanupShm(n.key());
  ASSERT_TRUE(n.Init());

  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    // 子进程独立 Init（走 OpenOnly 路径，复用同 key SHM）
    ConditionNotifier child;
    bool ok = child.Init();
    if (ok) {
      ReadableInfo got;
      ok = child.Listen(2000, &got);
      ok = ok && (got.host_id == 9999) && (got.channel_id == 7);
    }
    child.Shutdown();
    ::_exit(ok ? 0 : 1);
  }
  // 父进程稍等让子进入 Listen
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_TRUE(n.Notify({9999, 0, 7}));
  int status = 0;
  ::waitpid(pid, &status, 0);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  n.Shutdown();
  CleanupShm(n.key());
}

// fork：子 Notify，父 Listen 醒来（反向）
TEST(ConditionNotifierTest, ForkCrossProcessNotifyReverse) {
  ConditionNotifier n;
  CleanupShm(n.key());
  ASSERT_TRUE(n.Init());

  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    ConditionNotifier child;
    bool ok = child.Init();
    if (ok) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      ok = child.Notify({4242, 1, 9});
    }
    child.Shutdown();
    ::_exit(ok ? 0 : 1);
  }
  ReadableInfo got;
  ASSERT_TRUE(n.Listen(2000, &got));
  EXPECT_EQ(got.host_id, 4242u);
  EXPECT_EQ(got.channel_id, 9u);
  int status = 0;
  ::waitpid(pid, &status, 0);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  n.Shutdown();
  CleanupShm(n.key());
}