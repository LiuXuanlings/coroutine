#include <gtest/gtest.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "minicyber/transport/shm/condition_notifier.h"

using minicyber::transport::ConditionNotifier;

// Init 后两个 fd 都有效
TEST(ConditionNotifierTest, InitCreatesFds) {
  ConditionNotifier n;
  ASSERT_TRUE(n.Init());
  EXPECT_GE(n.Fd(), 0);
  EXPECT_GE(n.EpollFd(), 0);
  EXPECT_FALSE(n.IsShutdown());
  n.Shutdown();
}

// Fd() 多次调用返回同一个值
TEST(ConditionNotifierTest, FdIsStable) {
  ConditionNotifier n;
  ASSERT_TRUE(n.Init());
  int a = n.Fd();
  int b = n.Fd();
  EXPECT_EQ(a, b);
  n.Shutdown();
}

// Notify 后 Listen 立即返回 true
TEST(ConditionNotifierTest, NotifyThenListen) {
  ConditionNotifier n;
  ASSERT_TRUE(n.Init());
  EXPECT_TRUE(n.Notify());
  EXPECT_TRUE(n.Listen(100));
  n.Shutdown();
}

// 无通知时 Listen(0) 立即返回 false
TEST(ConditionNotifierTest, ListenNonBlockingNoNotify) {
  ConditionNotifier n;
  ASSERT_TRUE(n.Init());
  auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(n.Listen(0));
  auto elapsed = std::chrono::steady_clock::now() - start;
  // 非阻塞应在毫秒级返回
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 50);
  n.Shutdown();
}

// 无通知时 Listen(100) 约 100ms 后返回 false
TEST(ConditionNotifierTest, ListenTimeout) {
  ConditionNotifier n;
  ASSERT_TRUE(n.Init());
  auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(n.Listen(100));
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count();
  // 允许 20ms 抖动
  EXPECT_GE(elapsed, 80);
  EXPECT_LE(elapsed, 200);
  n.Shutdown();
}

// 多次 Notify 合并：3 次 write 后一次 Listen 即清空
TEST(ConditionNotifierTest, MultipleNotifyCoalesce) {
  ConditionNotifier n;
  ASSERT_TRUE(n.Init());
  ASSERT_TRUE(n.Notify());
  ASSERT_TRUE(n.Notify());
  ASSERT_TRUE(n.Notify());
  // 一次 Listen 应返回 true，且读出累加计数
  EXPECT_TRUE(n.Listen(100));
  // 计数已被 read 清空，再次 Listen(0) 应 false
  EXPECT_FALSE(n.Listen(0));
  n.Shutdown();
}

// Shutdown 后 Notify/Listen 失败
TEST(ConditionNotifierTest, ShutdownDisablesOps) {
  ConditionNotifier n;
  ASSERT_TRUE(n.Init());
  n.Shutdown();
  EXPECT_TRUE(n.IsShutdown());
  EXPECT_FALSE(n.Notify());
  EXPECT_FALSE(n.Listen(100));
  EXPECT_EQ(n.Fd(), -1);
  EXPECT_EQ(n.EpollFd(), -1);
}

// Shutdown 幂等
TEST(ConditionNotifierTest, ShutdownIdempotent) {
  ConditionNotifier n;
  ASSERT_TRUE(n.Init());
  n.Shutdown();
  n.Shutdown();
  n.Shutdown();
  SUCCEED();
}

// 析构自动 Shutdown
TEST(ConditionNotifierTest, DestructorAutoShutdown) {
  int fd = -1;
  {
    ConditionNotifier n;
    ASSERT_TRUE(n.Init());
    fd = n.Fd();
    EXPECT_GE(fd, 0);
  }
  // fd 已关闭，再次 close 应返回 -1 且 errno=EBADF
  EXPECT_EQ(::close(fd), -1);
}

// 重复 Init 安全（已初始化则直接成功）
TEST(ConditionNotifierTest, ReinitSafe) {
  ConditionNotifier n;
  ASSERT_TRUE(n.Init());
  int fd1 = n.Fd();
  ASSERT_TRUE(n.Init());
  EXPECT_EQ(n.Fd(), fd1);
  n.Shutdown();
}

// fork：父 Notify，子 Listen 醒来（验证 fd 可被子进程继承且唤醒有效）
TEST(ConditionNotifierTest, ForkCrossProcessNotify) {
  ConditionNotifier n;
  ASSERT_TRUE(n.Init());

  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    // 子进程：阻塞等待父进程通知（最多 1s）
    bool ok = n.Listen(1000);
    n.Shutdown();
    ::_exit(ok ? 0 : 1);
  }
  // 父进程稍等让子进入 epoll_wait
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_TRUE(n.Notify());
  int status = 0;
  ::waitpid(pid, &status, 0);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  n.Shutdown();
}

// fork：子 Notify，父 Listen 醒来（反向）
TEST(ConditionNotifierTest, ForkCrossProcessNotifyReverse) {
  ConditionNotifier n;
  ASSERT_TRUE(n.Init());

  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    // 子进程稍等父进入 Listen 后 Notify
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bool ok = n.Notify();
    n.Shutdown();
    ::_exit(ok ? 0 : 1);
  }
  // 父进程阻塞等待，最多 1s
  EXPECT_TRUE(n.Listen(1000));
  int status = 0;
  ::waitpid(pid, &status, 0);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  n.Shutdown();
}

// 高频 Notify/Listen 循环稳定
TEST(ConditionNotifierTest, HighFrequencyLoop) {
  ConditionNotifier n;
  ASSERT_TRUE(n.Init());
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(n.Notify()) << "i=" << i;
    ASSERT_TRUE(n.Listen(100)) << "i=" << i;
  }
  n.Shutdown();
}