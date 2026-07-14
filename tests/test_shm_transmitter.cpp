#include <gtest/gtest.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "minicyber/data/channel_buffer.h"
#include "minicyber/data/data_dispatcher.h"
#include "minicyber/data/data_notifier.h"
#include "minicyber/transport/dispatcher/shm_dispatcher.h"
#include "minicyber/transport/shm/condition_notifier.h"
#include "minicyber/transport/shm/posix_segment.h"
#include "minicyber/transport/transmitter/shm_transmitter.h"

using minicyber::data::ChannelBuffer;
using minicyber::data::DataDispatcher;
using minicyber::data::DataNotifier;
using minicyber::data::Notifier;
using minicyber::transport::ConditionNotifier;
using minicyber::transport::PosixSegment;
using minicyber::transport::ReadableInfo;
using minicyber::transport::ShmDispatcher;
using minicyber::transport::ShmTransmitter;
using minicyber::transport::ShmWritableBlock;

namespace {
void UnlinkShm(const std::string& name) {
  std::string path = "/dev/shm/" + name;
  ::unlink(path.c_str());
}
bool ShmFileExists(const std::string& name) {
  std::string path = "/dev/shm/" + name;
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}
}  // namespace

// 全局测试环境：清理可能残留的 notifier SHM（与 test_shm_dispatcher 一致）
class ShmTransmitterTestEnv : public ::testing::Environment {
 public:
  static void CleanupNotifierShm() {
    const char* p = "/minicyber/transport/shm/notifier";
    uint64_t h = 0;
    for (const char* c = p; *c; ++c) h = h * 131u + static_cast<uint64_t>(*c);
    key_t k = static_cast<key_t>(h & 0x7fffffff);
    int shmid = ::shmget(k, 0, 0644);
    if (shmid != -1) ::shmctl(shmid, IPC_RMID, 0);
  }
  void SetUp() override { CleanupNotifierShm(); }
  void TearDown() override { CleanupNotifierShm(); }
};

::testing::Environment* g_env =
    ::testing::AddGlobalTestEnvironment(new ShmTransmitterTestEnv);

// Enable 后 /dev/shm 文件出现
TEST(ShmTransmitterTest, EnableCreatesShmFile) {
  const uint64_t CH = 88001;
  UnlinkShm("minicyber_" + std::to_string(CH));
  ShmTransmitter tx(CH);//tx means transmitterX
  tx.Enable();
  EXPECT_TRUE(tx.enabled());
  EXPECT_TRUE(ShmFileExists("minicyber_" + std::to_string(CH)));
  tx.Disable();
  UnlinkShm("minicyber_" + std::to_string(CH));
}

// 未 Enable 时 Transmit 返回 false
TEST(ShmTransmitterTest, TransmitBeforeEnableReturnsFalse) {
  const uint64_t CH = 88002;
  ShmTransmitter tx(CH);
  EXPECT_FALSE(tx.Transmit(std::make_shared<std::string>("noop")));
  EXPECT_FALSE(tx.enabled());
}

// Disable 后 Transmit 返回 false
TEST(ShmTransmitterTest, DisableBlocksTransmit) {
  const uint64_t CH = 88003;
  UnlinkShm("minicyber_" + std::to_string(CH));
  ShmTransmitter tx(CH);
  tx.Enable();
  EXPECT_TRUE(tx.Transmit(std::make_shared<std::string>("first")));
  tx.Disable();
  EXPECT_FALSE(tx.Transmit(std::make_shared<std::string>("second")));
  UnlinkShm("minicyber_" + std::to_string(CH));
}

// Transmit 写入 payload 到 SHM block，直接读 PosixSegment 验证内容
TEST(ShmTransmitterTest, TransmitWritesPayloadToBlock) {
  const uint64_t CH = 88004;
  UnlinkShm("minicyber_" + std::to_string(CH));
  ShmTransmitter tx(CH);
  tx.Enable();
  ASSERT_TRUE(tx.Transmit(std::make_shared<std::string>("hello-shm")));

  // tx 内部 segment 是创建者；直接用 tx.segment() 读取
  PosixSegment* seg = tx.segment();
  ASSERT_NE(seg, nullptr);
  // seq 由 State::FetchAddSeq(1) 给出，首条消息 block_index = (seq-1) % block_num
  // block_num=4，首条 seq=1 -> index=0
  ShmWritableBlock rb;
  ASSERT_TRUE(seg->AcquireBlockToRead(0, &rb));
  EXPECT_EQ(rb.block->msg_size(), 9u);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(rb.buf), 9), "hello-shm");
  seg->ReleaseReadBlock(rb);

  tx.Disable();
  UnlinkShm("minicyber_" + std::to_string(CH));
}

// 多次 Transmit 递增 seq_num
TEST(ShmTransmitterTest, SeqNumIncrementsPerTransmit) {
  const uint64_t CH = 88005;
  UnlinkShm("minicyber_" + std::to_string(CH));
  ShmTransmitter tx(CH);
  tx.Enable();
  EXPECT_EQ(tx.seq_num(), 0u);
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(tx.Transmit(std::make_shared<std::string>(std::to_string(i))));
  }
  EXPECT_EQ(tx.seq_num(), 3u);
  tx.Disable();
  UnlinkShm("minicyber_" + std::to_string(CH));
}

// 端到端：ShmTransmitter + ShmDispatcher 同进程收发
TEST(ShmTransmitterTest, EndToEndSameProcess) {
  const uint64_t CH = 88006;
  UnlinkShm("minicyber_" + std::to_string(CH));

  // 接收侧：注册 ChannelBuffer + Notifier + AddSegment
  using BT = ChannelBuffer<std::string>::BufferType;
  auto buf = std::make_shared<BT>(10);
  ChannelBuffer<std::string> cb(CH, buf);
  DataDispatcher<std::string>::Instance()->AddBuffer(cb);

  std::atomic<int> fired{0};
  auto notifier = std::make_shared<Notifier>();
  notifier->callback = [&]() { ++fired; };
  DataNotifier::Instance()->AddNotifier(CH, notifier);

  ShmDispatcher::Instance()->AddSegment(CH);

  // 发送侧：ShmTransmitter
  ShmTransmitter tx(CH);
  tx.Enable();
  ASSERT_TRUE(tx.Transmit(std::make_shared<std::string>("e2e-msg")));

  // 等待 ShmDispatcher 后台线程读取并注入 DataDispatcher
  std::shared_ptr<std::string> got;
  for (int i = 0; i < 100; ++i) {
    uint64_t idx = 0;
    if (cb.Fetch(&idx, got)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(got != nullptr);
  EXPECT_EQ(*got, "e2e-msg");
  EXPECT_GE(fired.load(), 1);

  tx.Disable();
  UnlinkShm("minicyber_" + std::to_string(CH));
}

// 跨进程：fork 子进程用 ShmTransmitter 发送，父进程用 ShmDispatcher 接收
TEST(ShmTransmitterTest, ForkCrossProcessTransmit) {
  const uint64_t CH = 88007;
  UnlinkShm("minicyber_" + std::to_string(CH));

  // 父进程先创建 SHM 段（作为创建者），子进程 OpenOnly 同一段
  PosixSegment parent_seg(CH, 1024, 4);
  ASSERT_TRUE(parent_seg.Open());

  // 父进程注册订阅
  using BT = ChannelBuffer<std::string>::BufferType;
  auto buf = std::make_shared<BT>(10);
  ChannelBuffer<std::string> cb(CH, buf);
  DataDispatcher<std::string>::Instance()->AddBuffer(cb);

  ShmDispatcher::Instance()->AddSegment(CH);

  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    // 子进程：独立 ShmTransmitter（OpenOnly 同一段 SHM + 独立 ConditionNotifier）
    ShmTransmitter child_tx(CH);
    child_tx.Enable();
    if (!child_tx.enabled()) _exit(10);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!child_tx.Transmit(std::make_shared<std::string>("from-child")))
      _exit(12);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    child_tx.Disable();  // 不 Destroy，避免影响父进程的段
    _exit(0);
  }

  // 父进程等待消息到达
  std::shared_ptr<std::string> got;
  for (int i = 0; i < 200; ++i) {
    uint64_t idx = 0;
    if (cb.Fetch(&idx, got)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  ASSERT_TRUE(got != nullptr);
  EXPECT_EQ(*got, "from-child");

  parent_seg.Destroy();
  UnlinkShm("minicyber_" + std::to_string(CH));
}