#include <gtest/gtest.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "minicyber/transport/receiver/shm_receiver.h"
#include "minicyber/transport/transmitter/shm_transmitter.h"

using minicyber::transport::ShmReceiver;
using minicyber::transport::ShmTransmitter;

namespace {
void UnlinkShm(const std::string& name) {
  std::string path = "/dev/shm/" + name;
  ::unlink(path.c_str());
}

// 清理可能残留的 notifier SHM（与 test_shm_dispatcher 一致）
void CleanupNotifierShm() {
  const char* p = "/minicyber/transport/shm/notifier";
  uint64_t h = 0;
  for (const char* c = p; *c; ++c) h = h * 131u + static_cast<uint64_t>(*c);
  key_t k = static_cast<key_t>(h & 0x7fffffff);
  int shmid = ::shmget(k, 0, 0644);
  if (shmid != -1) ::shmctl(shmid, IPC_RMID, 0);
}

// 等待 receiver 回调收到消息，最多 wait_ms 毫秒
bool WaitForReceived(std::atomic<int>& counter, int target, int wait_ms) {
  for (int i = 0; i < wait_ms / 10; ++i) {
    if (counter.load() >= target) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return counter.load() >= target;
}
}  // namespace

// 全局测试环境：清理 notifier SHM
class ShmReceiverTestEnv : public ::testing::Environment {
 public:
  void SetUp() override { CleanupNotifierShm(); }
  void TearDown() override { CleanupNotifierShm(); }
};

::testing::Environment* g_env =
    ::testing::AddGlobalTestEnvironment(new ShmReceiverTestEnv);

// 同进程端到端：ShmTransmitter + ShmReceiver
TEST(ShmReceiverTest, SameProcessEndToEnd) {
  const uint64_t CH = 90001;
  UnlinkShm("minicyber_" + std::to_string(CH));

  std::atomic<int> count{0};
  std::string received;
  std::mutex mu;
  ShmReceiver rx(CH, [&](const std::shared_ptr<std::string>& msg) {
    ++count;
    std::lock_guard<std::mutex> lg(mu);
    received = *msg;
  });
  rx.Enable();

  ShmTransmitter tx(CH);
  tx.Enable();
  ASSERT_TRUE(tx.Transmit(std::make_shared<std::string>("e2e-shm")));

  ASSERT_TRUE(WaitForReceived(count, 1, 1000));
  EXPECT_EQ(received, "e2e-shm");

  tx.Disable();
  UnlinkShm("minicyber_" + std::to_string(CH));
}

// 未 Enable 时不收消息
TEST(ShmReceiverTest, NotEnabledNoCallback) {
  const uint64_t CH = 90002;
  UnlinkShm("minicyber_" + std::to_string(CH));

  std::atomic<int> count{0};
  ShmReceiver rx(CH, [&](const std::shared_ptr<std::string>&) { ++count; });
  // 未 Enable

  ShmTransmitter tx(CH);
  tx.Enable();
  tx.Transmit(std::make_shared<std::string>("ignored"));
  // 等一小段时间确认无回调
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(count.load(), 0);

  tx.Disable();
  UnlinkShm("minicyber_" + std::to_string(CH));
}

// Disable 后不再收到回调
TEST(ShmReceiverTest, DisableStopsCallbacks) {
  const uint64_t CH = 90003;
  UnlinkShm("minicyber_" + std::to_string(CH));

  std::atomic<int> count{0};
  ShmReceiver rx(CH, [&](const std::shared_ptr<std::string>&) { ++count; });
  rx.Enable();

  ShmTransmitter tx(CH);
  tx.Enable();
  tx.Transmit(std::make_shared<std::string>("first"));
  ASSERT_TRUE(WaitForReceived(count, 1, 1000));
  int after_first = count.load();

  rx.Disable();
  tx.Transmit(std::make_shared<std::string>("second"));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(count.load(), after_first);  // 不再递增

  tx.Disable();
  UnlinkShm("minicyber_" + std::to_string(CH));
}

TEST(ShmReceiverTest, DisableThenReenableRestoresSingleDelivery) {
  const uint64_t CH = 90008;
  UnlinkShm("minicyber_" + std::to_string(CH));
  std::atomic<int> count{0};
  ShmReceiver rx(CH, [&](const std::shared_ptr<std::string>&) { ++count; });
  ShmTransmitter tx(CH);
  rx.Enable();
  tx.Enable();
  ASSERT_TRUE(tx.Transmit(std::make_shared<std::string>("first")));
  ASSERT_TRUE(WaitForReceived(count, 1, 1000));

  rx.Disable();
  ASSERT_TRUE(tx.Transmit(std::make_shared<std::string>("ignored")));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(count.load(), 1);

  rx.Enable();
  ASSERT_TRUE(tx.Transmit(std::make_shared<std::string>("second")));
  ASSERT_TRUE(WaitForReceived(count, 2, 1000));
  EXPECT_EQ(count.load(), 2);
  tx.Disable();
  UnlinkShm("minicyber_" + std::to_string(CH));
}

// 多次发送，receiver 收到多条
TEST(ShmReceiverTest, MultipleMessages) {
  const uint64_t CH = 90004;
  UnlinkShm("minicyber_" + std::to_string(CH));

  std::atomic<int> count{0};
  std::vector<std::string> received;
  std::mutex mu;
  ShmReceiver rx(CH, [&](const std::shared_ptr<std::string>& msg) {
    int c = ++count;
    std::lock_guard<std::mutex> lg(mu);
    received.push_back(*msg);
    (void)c;
  });
  rx.Enable();

  ShmTransmitter tx(CH);
  tx.Enable();
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(tx.Transmit(std::make_shared<std::string>(std::to_string(i))));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  ASSERT_TRUE(WaitForReceived(count, 3, 1000));
  // 验证收到的消息内容（顺序可能因 block_num=4 轮转而交错，但每条都应存在）
  std::lock_guard<std::mutex> lg(mu);
  ASSERT_EQ(received.size(), 3u);
  std::set<std::string> got(received.begin(), received.end());
  EXPECT_EQ(got.count("0"), 1u);
  EXPECT_EQ(got.count("1"), 1u);
  EXPECT_EQ(got.count("2"), 1u);

  tx.Disable();
  UnlinkShm("minicyber_" + std::to_string(CH));
}

// 跨进程：fork 子进程 ShmTransmitter 发送，父进程 ShmReceiver 接收
TEST(ShmReceiverTest, ForkCrossProcessReceive) {
  const uint64_t CH = 90005;
  UnlinkShm("minicyber_" + std::to_string(CH));

  std::atomic<int> count{0};
  std::string received;
  std::mutex mu;
  ShmReceiver rx(CH, [&](const std::shared_ptr<std::string>& msg) {
    ++count;
    std::lock_guard<std::mutex> lg(mu);
    received = *msg;
  });
  rx.Enable();

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
    child_tx.Disable();  // 不 Destroy，避免影响父进程
    _exit(0);
  }

  // 父进程等待 receiver 回调
  ASSERT_TRUE(WaitForReceived(count, 1, 2000));
  int status = 0;
  ::waitpid(pid, &status, 0);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  EXPECT_EQ(received, "from-child");

  UnlinkShm("minicyber_" + std::to_string(CH));
}

// channel 隔离：CH_A 的发送不触发 CH_B 的 receiver
TEST(ShmReceiverTest, ChannelIsolation) {
  const uint64_t CH_A = 90006, CH_B = 90007;
  UnlinkShm("minicyber_" + std::to_string(CH_A));
  UnlinkShm("minicyber_" + std::to_string(CH_B));

  std::atomic<int> count_a{0}, count_b{0};
  std::string got_a, got_b;
  std::mutex mu;
  ShmReceiver rxa(CH_A, [&](const std::shared_ptr<std::string>& msg) {
    ++count_a;
    std::lock_guard<std::mutex> lg(mu);
    got_a = *msg;
  });
  ShmReceiver rxb(CH_B, [&](const std::shared_ptr<std::string>& msg) {
    ++count_b;
    std::lock_guard<std::mutex> lg(mu);
    got_b = *msg;
  });
  rxa.Enable();
  rxb.Enable();

  ShmTransmitter txa(CH_A), txb(CH_B);
  txa.Enable();
  txb.Enable();
  ASSERT_TRUE(txa.Transmit(std::make_shared<std::string>("A")));
  ASSERT_TRUE(txb.Transmit(std::make_shared<std::string>("B")));

  ASSERT_TRUE(WaitForReceived(count_a, 1, 1000));
  ASSERT_TRUE(WaitForReceived(count_b, 1, 1000));
  EXPECT_EQ(got_a, "A");
  EXPECT_EQ(got_b, "B");

  txa.Disable();
  txb.Disable();
  UnlinkShm("minicyber_" + std::to_string(CH_A));
  UnlinkShm("minicyber_" + std::to_string(CH_B));
}
