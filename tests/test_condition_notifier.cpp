#include <gtest/gtest.h>
#include <poll.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <array>
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

bool WriteByte(int fd) {
  const char value = 1;
  return ::write(fd, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value));
}

bool ReadByteBounded(int fd) {
  pollfd descriptor{fd, POLLIN, 0};
  if (::poll(&descriptor, 1, 2000) != 1) return false;
  char value = 0;
  return ::read(fd, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value));
}

bool WaitForChild(pid_t child, int* status) {
  if (status == nullptr) return false;
  for (int attempt = 0; attempt < 200; ++attempt) {
    const pid_t result = ::waitpid(child, status, WNOHANG);
    if (result == child) return true;
    if (result == -1) return false;
    ::poll(nullptr, 0, 10);
  }
  ::kill(child, SIGKILL);
  ::waitpid(child, status, 0);
  return false;
}

void DrainObservedNotifications(ConditionNotifier* notifier) {
  ReadableInfo ignored;
  while (notifier->Listen(0, &ignored)) {
  }
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
// 兜底极端繁忙的场景，不代表常态就是这样。
TEST(ConditionNotifierTest, ListenTimeout) {
  ConditionNotifier n;
  CleanupShm(n.key());
  ASSERT_TRUE(n.Init());
  // ConditionNotifier 使用与 CyberRT 一致的固定跨进程 key。其他已附着端点
  // 在 IPC_RMID 后的退出窗口仍可能写入旧观察者可见的通知；超时语义只约束
  // 排空当前观察窗口后没有后续通知的情况，不能把共享队列误判为私有空队列。
  DrainObservedNotifications(&n);
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

TEST(ConditionNotifierTest, ConcurrentPublishersCommitEveryNotification) {
  ConditionNotifier n;
  CleanupShm(n.key());
  ASSERT_TRUE(n.Init());
  constexpr uint32_t kPublishers = 4;
  constexpr uint32_t kMessagesPerPublisher = 32;
  std::vector<std::thread> publishers;
  for (uint32_t publisher = 0; publisher < kPublishers; ++publisher) {
    publishers.emplace_back([&n, publisher]() {
      for (uint32_t message = 0; message < kMessagesPerPublisher; ++message) {
        ASSERT_TRUE(n.Notify({publisher, message, 123}));
      }
    });
  }
  for (auto& publisher : publishers) {
    publisher.join();
  }

  std::array<uint32_t, kPublishers> received{};
  for (uint32_t i = 0; i < kPublishers * kMessagesPerPublisher; ++i) {
    ReadableInfo got;
    ASSERT_TRUE(n.Listen(100, &got));
    ASSERT_LT(got.host_id, kPublishers);
    ASSERT_EQ(got.channel_id, 123u);
    ++received[got.host_id];
  }
  for (uint32_t publisher = 0; publisher < kPublishers; ++publisher) {
    EXPECT_EQ(received[publisher], kMessagesPerPublisher);
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
    ASSERT_TRUE(n.Listen(100, &got)) << "k=" << k;//追加的自定义调试信息。当断言失败时，除了默认报错，还会额外输出
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

TEST(ConditionNotifierTest, InfiniteListenWaitsUntilNotification) {
  ConditionNotifier n;
  CleanupShm(n.key());
  ASSERT_TRUE(n.Init());

  ReadableInfo got;
  std::atomic<bool> received{false};
  std::thread listener([&]() {
    received.store(n.Listen(-1, &got), std::memory_order_release);
  });
  ASSERT_TRUE(n.Notify({77, 5, 88}));
  listener.join();

  EXPECT_TRUE(received.load(std::memory_order_acquire));
  EXPECT_EQ(got.host_id, 77u);
  EXPECT_EQ(got.block_index, 5u);
  EXPECT_EQ(got.channel_id, 88u);
  n.Shutdown();
  CleanupShm(n.key());
}

TEST(ConditionNotifierTest, ShutdownWaitsForInfiniteListenToExit) {
  ConditionNotifier n;
  CleanupShm(n.key());
  ASSERT_TRUE(n.Init());

  std::atomic<bool> listener_started{false};
  std::atomic<bool> listener_returned{false};
  std::thread listener([&]() {
    listener_started.store(true, std::memory_order_release);
    ReadableInfo ignored;
    EXPECT_FALSE(n.Listen(-1, &ignored));
    listener_returned.store(true, std::memory_order_release);
  });
  while (!listener_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  n.Shutdown();
  listener.join();
  EXPECT_TRUE(listener_returned.load(std::memory_order_acquire));
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

  int child_ready[2];
  int parent_go[2];
  ASSERT_EQ(::pipe(child_ready), 0);
  ASSERT_EQ(::pipe(parent_go), 0);

  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    ::close(child_ready[0]);
    ::close(parent_go[1]);
    // 子进程独立 Init（走 OpenOnly 路径，复用同 key SHM）
    ConditionNotifier child;
    bool ok = child.Init();
    ok = ok && WriteByte(child_ready[1]) && ReadByteBounded(parent_go[0]);
    if (ok) {
      ReadableInfo got;
      ok = child.Listen(2000, &got);
      ok = ok && (got.host_id == 9999) && (got.channel_id == 7);
    }
    child.Shutdown();
    ::_exit(ok ? 0 : 1);
  }
  ::close(child_ready[1]);
  ::close(parent_go[0]);
  ASSERT_TRUE(ReadByteBounded(child_ready[0]));
  ASSERT_TRUE(WriteByte(parent_go[1]));
  EXPECT_TRUE(n.Notify({9999, 0, 7}));
  int status = 0;
  ASSERT_TRUE(WaitForChild(pid, &status));
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  ::close(child_ready[0]);
  ::close(parent_go[1]);
  n.Shutdown();
  CleanupShm(n.key());
}

// fork：子 Notify，父 Listen 醒来（反向）
TEST(ConditionNotifierTest, ForkCrossProcessNotifyReverse) {
  ConditionNotifier n;
  CleanupShm(n.key());
  ASSERT_TRUE(n.Init());

  int child_ready[2];
  int child_go[2];
  ASSERT_EQ(::pipe(child_ready), 0);
  ASSERT_EQ(::pipe(child_go), 0);

  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    ::close(child_ready[0]);
    ::close(child_go[1]);
    ConditionNotifier child;
    bool ok = child.Init();
    ok = ok && WriteByte(child_ready[1]) && ReadByteBounded(child_go[0]);
    if (ok) ok = child.Notify({4242, 1, 9});
    child.Shutdown();
    ::_exit(ok ? 0 : 1);
  }
  ::close(child_ready[1]);
  ::close(child_go[0]);
  ASSERT_TRUE(ReadByteBounded(child_ready[0]));
  ASSERT_TRUE(WriteByte(child_go[1]));
  ReadableInfo got;
  ASSERT_TRUE(n.Listen(2000, &got));
  EXPECT_EQ(got.host_id, 4242u);
  EXPECT_EQ(got.channel_id, 9u);
  int status = 0;
  ASSERT_TRUE(WaitForChild(pid, &status));
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  ::close(child_ready[0]);
  ::close(child_go[1]);
  n.Shutdown();
  CleanupShm(n.key());
}
