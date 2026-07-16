#include <gtest/gtest.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <typeinfo>

#include "minicyber/topology/topology_manager.h"
#include "minicyber/transport/receiver/intra_receiver.h"
#include "minicyber/transport/receiver/shm_receiver.h"
#include "minicyber/transport/transmitter/intra_transmitter.h"
#include "minicyber/transport/transmitter/shm_transmitter.h"
#include "minicyber/transport/transport.h"

using minicyber::topology::TopologyManager;
using minicyber::transport::IntraReceiver;
using minicyber::transport::IntraTransmitter;
using minicyber::transport::ShmReceiver;
using minicyber::transport::ShmTransmitter;
using minicyber::transport::Transport;

namespace {
const pid_t kPidA = 1000;
const pid_t kPidB = 2000;

void UnlinkShm(const std::string& name) {
  std::string path = "/dev/shm/" + name;
  ::unlink(path.c_str());
}
void CleanupNotifierShm() {
  const char* p = "/minicyber/transport/shm/notifier";
  uint64_t h = 0;
  for (const char* c = p; *c; ++c) h = h * 131u + static_cast<uint64_t>(*c);
  key_t k = static_cast<key_t>(h & 0x7fffffff);
  int shmid = ::shmget(k, 0, 0644);
  if (shmid != -1) ::shmctl(shmid, IPC_RMID, 0);
}
}  // namespace

// 全局环境：清理 notifier SHM，避免残留影响跨进程测试
class TransportRoutingTestEnv : public ::testing::Environment {
 public:
  void SetUp() override { CleanupNotifierShm(); }
  void TearDown() override { CleanupNotifierShm(); }
};
::testing::Environment* g_env =
    ::testing::AddGlobalTestEnvironment(new TransportRoutingTestEnv);

// 同进程拓扑 -> CreateTransmitter<string> 返回 IntraTransmitter
TEST(TransportRoutingTest, SameProcReturnsIntraTransmitter) {
  const std::string CH = "/tr/intra_tx";
  auto* topo = TopologyManager::Instance();
  topo->AddChannelWriter(CH, "w", kPidA);
  topo->AddChannelReader(CH, "r", kPidA);

  auto tx = Transport::CreateTransmitter<std::string>(CH);
  ASSERT_NE(tx, nullptr);
  EXPECT_EQ(typeid(*tx).name(), typeid(IntraTransmitter<std::string>).name());
  EXPECT_TRUE(tx->enabled());
}

// 跨进程拓扑 -> CreateTransmitter<string> 返回 ShmTransmitter
TEST(TransportRoutingTest, DiffProcReturnsShmTransmitter) {
  const std::string CH = "/tr/shm_tx";
  UnlinkShm("minicyber_" + std::to_string(Transport::ChannelNameToId(CH)));
  auto* topo = TopologyManager::Instance();
  topo->AddChannelWriter(CH, "w", kPidA);
  topo->AddChannelReader(CH, "r", kPidB);

  auto tx = Transport::CreateTransmitter<std::string>(CH);
  ASSERT_NE(tx, nullptr);
  EXPECT_EQ(typeid(*tx).name(), typeid(ShmTransmitter).name());
  EXPECT_TRUE(tx->enabled());
}

// 同进程拓扑 -> CreateReceiver<string> 返回 IntraReceiver
TEST(TransportRoutingTest, SameProcReturnsIntraReceiver) {
  const std::string CH = "/tr/intra_rx";
  auto* topo = TopologyManager::Instance();
  topo->AddChannelWriter(CH, "w", kPidA);
  topo->AddChannelReader(CH, "r", kPidA);

  auto rx = Transport::CreateReceiver<std::string>(
      CH, [](const std::shared_ptr<std::string>&) {});
  ASSERT_NE(rx, nullptr);
  EXPECT_EQ(typeid(*rx).name(), typeid(IntraReceiver<std::string>).name());
  EXPECT_TRUE(rx->enabled());
}

// 跨进程拓扑 -> CreateReceiver<string> 返回 ShmReceiver
TEST(TransportRoutingTest, DiffProcReturnsShmReceiver) {
  const std::string CH = "/tr/shm_rx";
  UnlinkShm("minicyber_" + std::to_string(Transport::ChannelNameToId(CH)));
  auto* topo = TopologyManager::Instance();
  topo->AddChannelWriter(CH, "w", kPidA);
  topo->AddChannelReader(CH, "r", kPidB);

  auto rx = Transport::CreateReceiver<std::string>(
      CH, [](const std::shared_ptr<std::string>&) {});
  ASSERT_NE(rx, nullptr);
  EXPECT_EQ(typeid(*rx).name(), typeid(ShmReceiver).name());
  EXPECT_TRUE(rx->enabled());
}

// 同进程端到端：CreateTransmitter + CreateReceiver + Transmit -> 回调触发
TEST(TransportRoutingTest, SameProcEndToEnd) {
  const std::string CH = "/tr/e2e_intra";
  auto* topo = TopologyManager::Instance();
  topo->AddChannelWriter(CH, "w", kPidA);
  topo->AddChannelReader(CH, "r", kPidA);

  std::atomic<int> count{0};
  std::string received;
  std::mutex mu;
  auto rx = Transport::CreateReceiver<std::string>(
      CH, [&](const std::shared_ptr<std::string>& msg) {
        ++count;
        std::lock_guard<std::mutex> lg(mu);
        received = *msg;
      });
  auto tx = Transport::CreateTransmitter<std::string>(CH);
  ASSERT_TRUE(tx->Transmit(std::make_shared<std::string>("hello-routing")));

  // INTRA 是同步路径，回调应已触发
  EXPECT_GE(count.load(), 1);
  EXPECT_EQ(received, "hello-routing");
}

// 非 string 类型 -> 始终返回 Intra（SHM 暂不支持）
TEST(TransportRoutingTest, NonStringAlwaysIntra) {
  const std::string CH = "/tr/nonstring";
  auto* topo = TopologyManager::Instance();
  topo->AddChannelWriter(CH, "w", kPidA);
  topo->AddChannelReader(CH, "r", kPidB);  // 不同 pid，但 int 只能走 Intra

  auto tx = Transport::CreateTransmitter<int>(CH);
  ASSERT_NE(tx, nullptr);
  EXPECT_EQ(typeid(*tx).name(), typeid(IntraTransmitter<int>).name());
}

// 空拓扑（无 writer/reader 注册）-> IsSameProc false -> string 走 Shm
TEST(TransportRoutingTest, EmptyTopologyDefaultsToShm) {
  const std::string CH = "/tr/empty";
  UnlinkShm("minicyber_" + std::to_string(Transport::ChannelNameToId(CH)));

  auto tx = Transport::CreateTransmitter<std::string>(CH);
  ASSERT_NE(tx, nullptr);
  EXPECT_EQ(typeid(*tx).name(), typeid(ShmTransmitter).name());
}

// ChannelNameToId 对相同字符串产生相同 id
TEST(TransportRoutingTest, ChannelNameToIdConsistent) {
  uint64_t id1 = Transport::ChannelNameToId("/ch/consistent");
  uint64_t id2 = Transport::ChannelNameToId("/ch/consistent");
  EXPECT_EQ(id1, id2);
  uint64_t id3 = Transport::ChannelNameToId("/ch/different");
  EXPECT_NE(id1, id3);
}