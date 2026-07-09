#include <gtest/gtest.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "minicyber/transport/shm/posix_segment.h"

using minicyber::transport::PosixSegment;

namespace {
bool ShmFileExists(const std::string& name) {
  std::string path = "/dev/shm/" + name;
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}
void UnlinkShm(const std::string& name) {
  std::string path = "/dev/shm/" + name;
  ::unlink(path.c_str());
}
}  // namespace

// InstallSignalHandler 首次返回 true，后续返回 false
TEST(PosixSegmentSignalTest, InstallOnce) {
  PosixSegment::InstallSignalHandler();
  // 再调用应返回 false
  EXPECT_FALSE(PosixSegment::InstallSignalHandler());
}

// OpenOrCreate 成功后名字进入注册表
TEST(PosixSegmentSignalTest, RegisterOnCreate) {
  PosixSegment::ClearRegistryForTest();
  const uint64_t CH = 92001;
  UnlinkShm("minicyber_" + std::to_string(CH));
  PosixSegment seg(CH, 256, 2);
  ASSERT_TRUE(seg.Open());
  auto names = PosixSegment::RegisteredShmNames();
  ASSERT_EQ(names.size(), 1u);
  EXPECT_EQ(names[0], "minicyber_" + std::to_string(CH));
  seg.Destroy();
  // Destroy 后从注册表移除
  EXPECT_TRUE(PosixSegment::RegisteredShmNames().empty());
}

// OpenOnly 不注册（不是创建者）
TEST(PosixSegmentSignalTest, OpenOnlyDoesNotRegister) {
  PosixSegment::ClearRegistryForTest();
  const uint64_t CH = 92002;
  UnlinkShm("minicyber_" + std::to_string(CH));
  // 第一个 segment 创建并注册
  PosixSegment creator(CH, 256, 2);
  ASSERT_TRUE(creator.Open());
  ASSERT_EQ(PosixSegment::RegisteredShmNames().size(), 1u);

  // 第二个 OpenOnly 不应增加注册表
  PosixSegment opener(CH, 256, 2);
  ASSERT_TRUE(opener.Open());
  EXPECT_EQ(PosixSegment::RegisteredShmNames().size(), 1u);

  opener.Close();
  creator.Destroy();
}

// Destroy 移除注册表项
TEST(PosixSegmentSignalTest, DestroyUnregisters) {
  PosixSegment::ClearRegistryForTest();
  const uint64_t CH = 92003;
  UnlinkShm("minicyber_" + std::to_string(CH));
  {
    PosixSegment seg(CH, 256, 2);
    ASSERT_TRUE(seg.Open());
    EXPECT_EQ(PosixSegment::RegisteredShmNames().size(), 1u);
  }
  EXPECT_TRUE(PosixSegment::RegisteredShmNames().empty());
  EXPECT_FALSE(ShmFileExists("minicyber_" + std::to_string(CH)));
}

// 多个段同时注册
TEST(PosixSegmentSignalTest, MultipleRegistrations) {
  PosixSegment::ClearRegistryForTest();
  const uint64_t CH1 = 92004, CH2 = 92005, CH3 = 92006;
  UnlinkShm("minicyber_" + std::to_string(CH1));
  UnlinkShm("minicyber_" + std::to_string(CH2));
  UnlinkShm("minicyber_" + std::to_string(CH3));

  PosixSegment s1(CH1, 256, 1);
  PosixSegment s2(CH2, 256, 1);
  PosixSegment s3(CH3, 256, 1);
  ASSERT_TRUE(s1.Open());
  ASSERT_TRUE(s2.Open());
  ASSERT_TRUE(s3.Open());
  EXPECT_EQ(PosixSegment::RegisteredShmNames().size(), 3u);

  s2.Destroy();
  auto names = PosixSegment::RegisteredShmNames();
  ASSERT_EQ(names.size(), 2u);
  // 剩下两个应恰好是 CH1 和 CH3（顺序无关）
  bool has1 = false, has3 = false;
  for (auto& n : names) {
    if (n == "minicyber_" + std::to_string(CH1)) has1 = true;
    if (n == "minicyber_" + std::to_string(CH3)) has3 = true;
  }
  EXPECT_TRUE(has1);
  EXPECT_TRUE(has3);

  s1.Destroy();
  s3.Destroy();
  EXPECT_TRUE(PosixSegment::RegisteredShmNames().empty());
}

// CleanupAllForTest 清理所有注册段
TEST(PosixSegmentSignalTest, CleanupAll) {
  PosixSegment::ClearRegistryForTest();
  const uint64_t CH1 = 92007, CH2 = 92008;
  UnlinkShm("minicyber_" + std::to_string(CH1));
  UnlinkShm("minicyber_" + std::to_string(CH2));

  PosixSegment s1(CH1, 256, 1);
  PosixSegment s2(CH2, 256, 1);
  ASSERT_TRUE(s1.Open());
  ASSERT_TRUE(s2.Open());
  ASSERT_TRUE(ShmFileExists("minicyber_" + std::to_string(CH1)));
  ASSERT_TRUE(ShmFileExists("minicyber_" + std::to_string(CH2)));

  int n = PosixSegment::CleanupAllForTest();
  EXPECT_EQ(n, 2);
  EXPECT_FALSE(ShmFileExists("minicyber_" + std::to_string(CH1)));
  EXPECT_FALSE(ShmFileExists("minicyber_" + std::to_string(CH2)));
}

// 关键测试：fork 子进程创建段后 raise(SIGTERM)，
// 验证信号处理器自动 shm_unlink，/dev/shm 文件消失
TEST(PosixSegmentSignalTest, ForkChildSigtermAutoCleanup) {
  const uint64_t CH = 92009;
  UnlinkShm("minicyber_" + std::to_string(CH));

  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    // 子进程：创建段，然后自杀
    PosixSegment seg(CH, 256, 1);
    if (!seg.Open()) _exit(3);
    if (!ShmFileExists("minicyber_" + std::to_string(CH))) _exit(4);
    // 触发 SIGTERM，应被 CrashHandler 捕获并清理
    ::raise(SIGTERM);
    // 不应执行到这里
    _exit(5);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  ASSERT_TRUE(WIFSIGNALED(status)) << "status=" << status;
  EXPECT_EQ(WTERMSIG(status), SIGTERM);
  // 关键断言：信号处理器已 shm_unlink
  EXPECT_FALSE(ShmFileExists("minicyber_" + std::to_string(CH)));
  // 兜底清理
  UnlinkShm("minicyber_" + std::to_string(CH));
}

// 同样验证 SIGINT
TEST(PosixSegmentSignalTest, ForkChildSigintAutoCleanup) {
  const uint64_t CH = 92010;
  UnlinkShm("minicyber_" + std::to_string(CH));

  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    PosixSegment seg(CH, 256, 1);
    if (!seg.Open()) _exit(3);
    ::raise(SIGINT);
    _exit(5);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGINT);
  EXPECT_FALSE(ShmFileExists("minicyber_" + std::to_string(CH)));
  UnlinkShm("minicyber_" + std::to_string(CH));
}

// 子进程正常退出（exit(0)）不应触发清理；父进程仍可见段
TEST(PosixSegmentSignalTest, NormalExitDoesNotCleanup) {
  const uint64_t CH = 92011;
  UnlinkShm("minicyber_" + std::to_string(CH));

  // 父进程创建段
  PosixSegment seg(CH, 256, 1);
  ASSERT_TRUE(seg.Open());
  ASSERT_TRUE(ShmFileExists("minicyber_" + std::to_string(CH)));

  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    // 子进程只是 OpenOnly，然后正常退出
    PosixSegment child(CH, 256, 1);
    if (!child.Open()) _exit(3);
    child.Close();
    _exit(0);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  // 段仍在（父进程持有）
  EXPECT_TRUE(ShmFileExists("minicyber_" + std::to_string(CH)));
  seg.Destroy();
}