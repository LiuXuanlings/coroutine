#include <gtest/gtest.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <thread>

#include "minicyber/transport/shm/posix_segment.h"

using minicyber::transport::Block;
using minicyber::transport::PosixSegment;
using minicyber::transport::State;

// PosixSegment Close / Destroy 简要区分
// Close()：仅断开当前进程对共享内存的映射、关闭 shm 文件描述符，不删除全局共享内存文件
// Destroy()：彻底销毁共享内存：断开映射 + 关闭 fd + 删除全局共享内存文件
// 只用 Close() 场景：临时关闭共享内存，后续需要重新打开；多进程场景，单个进程退出，不销毁公共通道；
// 必须用 Destroy() 场景：业务逻辑通道生命周期结束，不再使用；单元测试收尾，清除 /dev/shm 残留文件，避免用例互相干扰

namespace {
// 检查 /dev/shm 下文件是否存在
bool ShmFileExists(const std::string& name) {
  std::string path = "/dev/shm/" + name;
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

// 删除残留 SHM 文件，避免上一次测试未清理影响
void UnlinkShm(const std::string& name) {
  std::string path = "/dev/shm/" + name;
  ::unlink(path.c_str());
}
}  // namespace

// 构造时记录 channel_id 与默认参数
TEST(PosixSegmentTest, Construct) {
  PosixSegment seg(1234);
  EXPECT_EQ(seg.channel_id(), 1234u);
  EXPECT_EQ(seg.shm_name(), "minicyber_1234");
  EXPECT_EQ(seg.ceiling_msg_size(), 1024u);
  EXPECT_EQ(seg.block_num(), 4u);
}

// Open 后 /dev/shm 文件出现，GetMemPtr/GetSize 非空
TEST(PosixSegmentTest, OpenCreatesShmFile) {
  const uint64_t CH = 91001;
  UnlinkShm("minicyber_" + std::to_string(CH));
  PosixSegment seg(CH, 256, 2);
  ASSERT_TRUE(seg.Open());
  EXPECT_NE(seg.GetMemPtr(), nullptr);
  EXPECT_GT(seg.GetSize(), 0u);
  EXPECT_TRUE(ShmFileExists("minicyber_" + std::to_string(CH)));
  seg.Destroy();
  EXPECT_FALSE(ShmFileExists("minicyber_" + std::to_string(CH)));
}

// State 与 Block 正确放置
TEST(PosixSegmentTest, StateAndBlockLayout) {
  const uint64_t CH = 91002;
  UnlinkShm("minicyber_" + std::to_string(CH));
  PosixSegment seg(CH, 256, 3);
  ASSERT_TRUE(seg.Open());
  EXPECT_NE(seg.state(), nullptr);
  EXPECT_NE(seg.blocks(), nullptr);
  EXPECT_EQ(seg.state()->ceiling_msg_size(), 256u);
  EXPECT_EQ(seg.block_num(), 3u);
  seg.Destroy();
}

// 通过 GetMemPtr 写入数据，可读回
TEST(PosixSegmentTest, WriteReadThroughMemPtr) {
  const uint64_t CH = 91003;
  UnlinkShm("minicyber_" + std::to_string(CH));
  PosixSegment seg(CH, 256, 2);
  ASSERT_TRUE(seg.Open());
  auto* p = static_cast<uint8_t*>(seg.GetMemPtr());
  // 注意：前 sizeof(State) 字节是控制区，写 payload 区
  uint8_t* payload = p + sizeof(State);
  for (size_t i = 0; i < 16; ++i) payload[i] = static_cast<uint8_t>(0x80 + i);
  for (size_t i = 0; i < 16; ++i) {
    EXPECT_EQ(payload[i], static_cast<uint8_t>(0x80 + i));
  }
  seg.Destroy();
}

// Open 幂等
TEST(PosixSegmentTest, OpenIsIdempotent) {
  const uint64_t CH = 91004;
  UnlinkShm("minicyber_" + std::to_string(CH));
  PosixSegment seg(CH, 256, 2);
  ASSERT_TRUE(seg.Open());
  void* first = seg.GetMemPtr();
  ASSERT_TRUE(seg.Open());
  EXPECT_EQ(seg.GetMemPtr(), first);
  seg.Destroy();
}

// Close 后 GetMemPtr 为空且可重新 Open
TEST(PosixSegmentTest, CloseAndReopen) {
  const uint64_t CH = 91005;
  UnlinkShm("minicyber_" + std::to_string(CH));
  PosixSegment seg(CH, 256, 2);
  ASSERT_TRUE(seg.Open());
  seg.Close();
  EXPECT_EQ(seg.GetMemPtr(), nullptr);
  // 文件还在
  EXPECT_TRUE(ShmFileExists("minicyber_" + std::to_string(CH)));
  ASSERT_TRUE(seg.Open());
  EXPECT_NE(seg.GetMemPtr(), nullptr);
  seg.Destroy();
}

// Close 幂等
TEST(PosixSegmentTest, CloseIsIdempotent) {
  const uint64_t CH = 91006;
  UnlinkShm("minicyber_" + std::to_string(CH));
  PosixSegment seg(CH, 256, 2);
  ASSERT_TRUE(seg.Open());
  seg.Close();
  seg.Close();
  SUCCEED();
}

// Destroy 后 /dev/shm 文件消失
TEST(PosixSegmentTest, DestroyRemovesFile) {
  const uint64_t CH = 91007;
  UnlinkShm("minicyber_" + std::to_string(CH));
  {
    PosixSegment seg(CH, 256, 2);
    ASSERT_TRUE(seg.Open());
    EXPECT_TRUE(ShmFileExists("minicyber_" + std::to_string(CH)));
  }
  EXPECT_FALSE(ShmFileExists("minicyber_" + std::to_string(CH)));
}

// 同 channel 两个 PosixSegment：第二个走 OpenOnly，引用计数=2，
// 且两段映射指向同一物理内存（不同虚拟地址，但内容共享）
TEST(PosixSegmentTest, TwoSegmentsSameChannelOpenOnly) {
  const uint64_t CH = 91008;
  UnlinkShm("minicyber_" + std::to_string(CH));
  PosixSegment seg1(CH, 256, 2);
  ASSERT_TRUE(seg1.Open());
  EXPECT_EQ(seg1.state()->reference_counts(), 1u);

  PosixSegment seg2(CH, 256, 2);
  ASSERT_TRUE(seg2.Open());
  // 两份映射的引用计数都应是 2（指向同一个 State）
  EXPECT_EQ(seg1.state()->reference_counts(), 2u);
  EXPECT_EQ(seg2.state()->reference_counts(), 2u);
  // 虚拟地址可能不同，但物理内存是同一段：通过 seg1 写，通过 seg2 读应可见
  auto* p1 = static_cast<uint8_t*>(seg1.GetMemPtr()) + sizeof(State);
  auto* p2 = static_cast<uint8_t*>(seg2.GetMemPtr()) + sizeof(State);
  const uint32_t VAL = 0x12345678;
  std::memcpy(p1, &VAL, sizeof(VAL));
  uint32_t got = 0;
  std::memcpy(&got, p2, sizeof(got));
  EXPECT_EQ(got, VAL);

  seg2.Close();
  EXPECT_EQ(seg1.state()->reference_counts(), 1u);
  seg1.Destroy();
}

// fork：父进程写，子进程读，验证跨进程可见
TEST(PosixSegmentTest, ForkCrossProcessRead) {
  const uint64_t CH = 91009;
  UnlinkShm("minicyber_" + std::to_string(CH));
  PosixSegment seg(CH, 256, 2);
  ASSERT_TRUE(seg.Open());

  // 父进程在 payload 区写一个魔数
  auto* base = static_cast<uint8_t*>(seg.GetMemPtr());
  uint8_t* payload = base + sizeof(State);
  const uint32_t MAGIC = 0xDEADBEEF;
  std::memcpy(payload, &MAGIC, sizeof(MAGIC));

  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    // 子进程：打开同一段共享内存，读魔数
    PosixSegment child_seg(CH, 256, 2);
    if (!child_seg.Open()) _exit(1);
    uint8_t* cp = static_cast<uint8_t*>(child_seg.GetMemPtr()) + sizeof(State);
    uint32_t got = 0;
    std::memcpy(&got, cp, sizeof(got));
    child_seg.Close();
    // 注意：不要 Destroy，否则影响父进程
    _exit(got == MAGIC ? 0 : 2);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  EXPECT_TRUE(WIFEXITED(status));// 子进程正常退出
  EXPECT_EQ(WEXITSTATUS(status), 0);// 子进程读到正确魔数
  seg.Destroy();
}

// fork：子进程写，父进程读，验证双向
TEST(PosixSegmentTest, ForkCrossProcessWrite) {
  const uint64_t CH = 91010;
  UnlinkShm("minicyber_" + std::to_string(CH));
  PosixSegment seg(CH, 256, 2);
  ASSERT_TRUE(seg.Open());

  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    PosixSegment child_seg(CH, 256, 2);
    if (!child_seg.Open()) _exit(1);
    uint8_t* cp = static_cast<uint8_t*>(child_seg.GetMemPtr()) + sizeof(State);
    const uint32_t VAL = 0xCAFEBABE;
    std::memcpy(cp, &VAL, sizeof(VAL));
    child_seg.Close();
    _exit(0);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  uint8_t* payload = static_cast<uint8_t*>(seg.GetMemPtr()) + sizeof(State);
  uint32_t got = 0;
  std::memcpy(&got, payload, sizeof(got));
  EXPECT_EQ(got, 0xCAFEBABEu);
  seg.Destroy();
}

TEST(PosixSegmentTest, WriteAcquisitionSkipsBusyBlocks) {
  const uint64_t CH = 91011;
  UnlinkShm("minicyber_" + std::to_string(CH));
  PosixSegment seg(CH, 64, 2);
  ASSERT_TRUE(seg.Open());

  minicyber::transport::ShmWritableBlock first;
  minicyber::transport::ShmWritableBlock second;
  minicyber::transport::ShmWritableBlock third;
  ASSERT_TRUE(seg.AcquireBlockToWrite(8, &first));
  ASSERT_TRUE(seg.AcquireBlockToWrite(8, &second));
  EXPECT_NE(first.index, second.index);
  EXPECT_FALSE(seg.AcquireBlockToWrite(8, &third));

  seg.ReleaseWrittenBlock(first);
  ASSERT_TRUE(seg.AcquireBlockToWrite(8, &third));
  seg.ReleaseWrittenBlock(second);
  seg.ReleaseWrittenBlock(third);
  seg.Destroy();
}

TEST(PosixSegmentTest, ReleaseRejectsForeignBlockView) {
  const uint64_t CH = 91012;
  UnlinkShm("minicyber_" + std::to_string(CH));
  PosixSegment seg(CH, 64, 1);
  ASSERT_TRUE(seg.Open());

  minicyber::transport::ShmWritableBlock writable;
  ASSERT_TRUE(seg.AcquireBlockToWrite(8, &writable));
  minicyber::transport::ShmWritableBlock foreign = writable;
  Block unrelated;
  foreign.block = &unrelated;
  seg.ReleaseWrittenBlock(foreign);

  minicyber::transport::ShmWritableBlock blocked;
  EXPECT_FALSE(seg.AcquireBlockToWrite(8, &blocked));
  seg.ReleaseWrittenBlock(writable);
  EXPECT_TRUE(seg.AcquireBlockToWrite(8, &blocked));
  seg.ReleaseWrittenBlock(blocked);
  seg.Destroy();
}
