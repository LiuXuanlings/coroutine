#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "minicyber/proto/role_attributes.pb.h"
#include "minicyber/topology/topology_manager.h"
#include "minicyber/transport/receiver/shm_receiver.h"
#include "minicyber/transport/shm/posix_segment.h"
#include "minicyber/transport/transport.h"
#include "minicyber/transport/transmitter/shm_transmitter.h"

namespace {

using minicyber::proto::RoleAttributes;
using minicyber::topology::TopologyManager;
using minicyber::transport::ShmReceiver;
using minicyber::transport::ShmTransmitter;
using minicyber::transport::Transport;

std::string LocalHostName() {
  char host_name[256] = {};
  EXPECT_EQ(::gethostname(host_name, sizeof(host_name) - 1), 0);
  return host_name;
}

RoleAttributes MakeRole(const std::string& channel, const std::string& node_name,
                        pid_t process_id, uint64_t id) {
  RoleAttributes attr;
  attr.set_host_name(LocalHostName());
  attr.set_process_id(process_id);
  attr.set_node_name(node_name);
  attr.set_channel_name(channel);
  attr.set_channel_id(Transport::ChannelNameToId(channel));
  attr.set_id(id);
  return attr;
}

bool WaitFor(const std::atomic<int>& count, int expected, int timeout_ms = 2000) {
  for (int elapsed = 0; elapsed < timeout_ms; elapsed += 10) {
    if (count.load(std::memory_order_acquire) >= expected) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return count.load(std::memory_order_acquire) >= expected;
}

void CleanupChannel(const std::string& channel) {
  const std::string path = "/dev/shm/minicyber_" +
                           std::to_string(Transport::ChannelNameToId(channel));
  ::unlink(path.c_str());
}

class HybridTransportTest : public ::testing::Test {
 protected:
  void TearDown() override { TopologyManager::Instance()->Shutdown(); }
};

TEST_F(HybridTransportTest, SameProcUsesOriginalSharedPtr) {
  const std::string channel = "/hybrid/intra";
  const auto writer = MakeRole(channel, "intra_writer", ::getpid(), 1001);
  const auto reader = MakeRole(channel, "intra_reader", ::getpid(), 1002);

  std::atomic<int> received{0};
  std::shared_ptr<RoleAttributes> delivered;
  auto rx = Transport::CreateHybridReceiver<RoleAttributes>(
      reader, [&](const std::shared_ptr<RoleAttributes>& message) {
        delivered = message;
        received.fetch_add(1, std::memory_order_release);
      });
  auto tx = Transport::CreateHybridTransmitter<RoleAttributes>(writer);

  ASSERT_TRUE(TopologyManager::Instance()->Join(minicyber::proto::ROLE_WRITER,
                                                 writer));
  ASSERT_TRUE(TopologyManager::Instance()->Join(minicyber::proto::ROLE_READER,
                                                 reader));
  ASSERT_TRUE(tx->HasReader());

  auto message = std::make_shared<RoleAttributes>();
  message->set_node_name("shared-object");
  ASSERT_TRUE(tx->Transmit(message));
  ASSERT_EQ(received.load(std::memory_order_acquire), 1);
  EXPECT_EQ(delivered.get(), message.get());
  EXPECT_EQ(delivered->node_name(), "shared-object");

  ASSERT_TRUE(TopologyManager::Instance()->Leave(minicyber::proto::ROLE_READER,
                                                  reader));
  EXPECT_FALSE(tx->HasReader());
  tx->Disable();
  rx->Disable();
}

TEST_F(HybridTransportTest, IntraCallbackCanDisableTransmitter) {
  const std::string channel = "/hybrid/reentrant_tx_shutdown";
  const auto writer = MakeRole(channel, "reentrant_writer", ::getpid(), 1101);
  const auto reader = MakeRole(channel, "reentrant_reader", ::getpid(), 1102);

  std::shared_ptr<minicyber::transport::HybridTransmitter<RoleAttributes>> tx;
  std::atomic<int> callbacks{0};
  auto rx = Transport::CreateHybridReceiver<RoleAttributes>(
      reader, [&](const std::shared_ptr<RoleAttributes>&) {
        callbacks.fetch_add(1, std::memory_order_release);
        tx->Disable();
      });
  tx = Transport::CreateHybridTransmitter<RoleAttributes>(writer);
  ASSERT_TRUE(TopologyManager::Instance()->Join(minicyber::proto::ROLE_WRITER,
                                                 writer));
  ASSERT_TRUE(TopologyManager::Instance()->Join(minicyber::proto::ROLE_READER,
                                                 reader));

  EXPECT_TRUE(tx->Transmit(std::make_shared<RoleAttributes>()));
  EXPECT_EQ(callbacks.load(std::memory_order_acquire), 1);
  EXPECT_FALSE(tx->enabled());
  rx->Disable();
}

TEST_F(HybridTransportTest, IntraCallbackCanDisableReceiver) {
  const std::string channel = "/hybrid/reentrant_rx_shutdown";
  const auto writer = MakeRole(channel, "reentrant_writer", ::getpid(), 1201);
  const auto reader = MakeRole(channel, "reentrant_reader", ::getpid(), 1202);

  std::shared_ptr<minicyber::transport::HybridReceiver<RoleAttributes>> rx;
  std::atomic<int> callbacks{0};
  rx = Transport::CreateHybridReceiver<RoleAttributes>(
      reader, [&](const std::shared_ptr<RoleAttributes>&) {
        callbacks.fetch_add(1, std::memory_order_release);
        rx->Disable();
      });
  auto tx = Transport::CreateHybridTransmitter<RoleAttributes>(writer);
  ASSERT_TRUE(TopologyManager::Instance()->Join(minicyber::proto::ROLE_WRITER,
                                                 writer));
  ASSERT_TRUE(TopologyManager::Instance()->Join(minicyber::proto::ROLE_READER,
                                                 reader));

  EXPECT_TRUE(tx->Transmit(std::make_shared<RoleAttributes>()));
  EXPECT_EQ(callbacks.load(std::memory_order_acquire), 1);
  EXPECT_FALSE(rx->enabled());
  tx->Disable();
}

TEST_F(HybridTransportTest, DiffProcSerializesProtobufWithDynamicFields) {
  const std::string channel = "/hybrid/shm";
  CleanupChannel(channel);
  const auto writer = MakeRole(channel, "shm_writer", ::getpid(), 2001);
  const auto reader = MakeRole(channel, "shm_reader", ::getpid() + 1000, 2002);

  std::atomic<int> received{0};
  std::shared_ptr<RoleAttributes> delivered;
  auto rx = Transport::CreateHybridReceiver<RoleAttributes>(
      reader, [&](const std::shared_ptr<RoleAttributes>& message) {
        delivered = message;
        received.fetch_add(1, std::memory_order_release);
      });
  auto tx = Transport::CreateHybridTransmitter<RoleAttributes>(writer);

  ASSERT_TRUE(TopologyManager::Instance()->Join(minicyber::proto::ROLE_WRITER,
                                                 writer));
  ASSERT_TRUE(TopologyManager::Instance()->Join(minicyber::proto::ROLE_READER,
                                                 reader));
  ASSERT_TRUE(tx->HasReader());

  auto message = std::make_shared<RoleAttributes>();
  message->set_node_name("serialized-object");
  message->set_message_type("minicyber.test.DynamicPayload");
  message->set_proto_desc(std::string(2048, 'p'));
  ASSERT_TRUE(tx->Transmit(message));
  ASSERT_TRUE(WaitFor(received, 1));
  ASSERT_NE(delivered, nullptr);
  EXPECT_NE(delivered.get(), message.get());
  EXPECT_EQ(delivered->node_name(), message->node_name());
  EXPECT_EQ(delivered->message_type(), message->message_type());
  EXPECT_EQ(delivered->proto_desc(), message->proto_desc());

  tx->Disable();
  rx->Disable();
  CleanupChannel(channel);
}

TEST_F(HybridTransportTest, ProtobufShmCrossProcessRoundTrip) {
  const std::string channel = "/hybrid/protobuf_cross_process";
  CleanupChannel(channel);
  const uint64_t channel_id = Transport::ChannelNameToId(channel);

  std::atomic<int> received{0};
  std::shared_ptr<RoleAttributes> delivered;
  ShmReceiver<RoleAttributes> receiver(
      channel_id, [&](const std::shared_ptr<RoleAttributes>& message) {
        delivered = message;
        received.fetch_add(1, std::memory_order_release);
      });
  receiver.Enable();
  ASSERT_TRUE(receiver.enabled());

  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    ShmTransmitter<RoleAttributes> transmitter(channel_id);
    transmitter.Enable();
    if (!transmitter.enabled()) _exit(10);
    auto message = std::make_shared<RoleAttributes>();
    message->set_node_name("forked-protobuf-writer");
    message->set_proto_desc(std::string("child\0bytes", 11));
    const bool transmitted = transmitter.Transmit(message);
    transmitter.Disable();
    _exit(transmitted ? 0 : 11);
  }

  ASSERT_TRUE(WaitFor(received, 1));
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
  ASSERT_NE(delivered, nullptr);
  EXPECT_EQ(delivered->node_name(), "forked-protobuf-writer");
  EXPECT_EQ(delivered->proto_desc(), std::string("child\0bytes", 11));

  receiver.Disable();
  CleanupChannel(channel);
}

TEST_F(HybridTransportTest, ProtobufShmDeliversEmptyMessage) {
  const std::string channel = "/hybrid/protobuf_empty";
  CleanupChannel(channel);
  const uint64_t channel_id = Transport::ChannelNameToId(channel);

  std::atomic<int> received{0};
  ShmReceiver<RoleAttributes> receiver(
      channel_id, [&](const std::shared_ptr<RoleAttributes>& message) {
        EXPECT_TRUE(message->IsInitialized());
        received.fetch_add(1, std::memory_order_release);
      });
  ShmTransmitter<RoleAttributes> transmitter(channel_id);
  receiver.Enable();
  transmitter.Enable();
  ASSERT_TRUE(transmitter.Transmit(std::make_shared<RoleAttributes>()));
  EXPECT_TRUE(WaitFor(received, 1));

  transmitter.Disable();
  receiver.Disable();
  CleanupChannel(channel);
}

TEST_F(HybridTransportTest,
       ProtobufReceiverRejectsIncompatibleExistingSegment) {
  const std::string channel = "/hybrid/incompatible_segment";
  CleanupChannel(channel);
  const uint64_t channel_id = Transport::ChannelNameToId(channel);
  minicyber::transport::PosixSegment small_segment(channel_id, 1024, 4);
  ASSERT_TRUE(small_segment.Open());

  ShmReceiver<RoleAttributes> receiver(
      channel_id, [](const std::shared_ptr<RoleAttributes>&) {});
  receiver.Enable();
  EXPECT_FALSE(receiver.enabled());

  receiver.Disable();
  small_segment.Destroy();
  CleanupChannel(channel);
}

TEST_F(HybridTransportTest, MixedOppositesFanOutWithoutDuplicateDelivery) {
  const std::string channel = "/hybrid/mixed";
  CleanupChannel(channel);
  const auto writer = MakeRole(channel, "mixed_writer", ::getpid(), 3001);
  const auto local_reader =
      MakeRole(channel, "mixed_local_reader", ::getpid(), 3002);
  const auto remote_reader =
      MakeRole(channel, "mixed_remote_reader", ::getpid() + 2000, 3003);

  std::atomic<int> local_count{0};
  std::atomic<int> remote_count{0};
  std::shared_ptr<RoleAttributes> local_message;
  std::shared_ptr<RoleAttributes> remote_message;
  auto local_rx = Transport::CreateHybridReceiver<RoleAttributes>(
      local_reader, [&](const std::shared_ptr<RoleAttributes>& message) {
        local_message = message;
        local_count.fetch_add(1, std::memory_order_release);
      });
  auto remote_rx = Transport::CreateHybridReceiver<RoleAttributes>(
      remote_reader, [&](const std::shared_ptr<RoleAttributes>& message) {
        remote_message = message;
        remote_count.fetch_add(1, std::memory_order_release);
      });
  auto tx = Transport::CreateHybridTransmitter<RoleAttributes>(writer);

  ASSERT_TRUE(TopologyManager::Instance()->Join(minicyber::proto::ROLE_WRITER,
                                                 writer));
  ASSERT_TRUE(TopologyManager::Instance()->Join(minicyber::proto::ROLE_READER,
                                                 local_reader));
  ASSERT_TRUE(TopologyManager::Instance()->Join(minicyber::proto::ROLE_READER,
                                                 remote_reader));

  auto message = std::make_shared<RoleAttributes>();
  message->set_node_name("hybrid-fanout");
  ASSERT_TRUE(tx->Transmit(message));
  ASSERT_EQ(local_count.load(std::memory_order_acquire), 1);
  ASSERT_TRUE(WaitFor(remote_count, 1));
  EXPECT_EQ(local_message.get(), message.get());
  EXPECT_NE(remote_message.get(), message.get());
  EXPECT_EQ(remote_message->node_name(), message->node_name());
  EXPECT_EQ(local_count.load(std::memory_order_acquire), 1);
  EXPECT_EQ(remote_count.load(std::memory_order_acquire), 1);

  ASSERT_TRUE(TopologyManager::Instance()->Leave(minicyber::proto::ROLE_READER,
                                                  remote_reader));
  ASSERT_TRUE(tx->Transmit(message));
  EXPECT_EQ(local_count.load(std::memory_order_acquire), 2);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(remote_count.load(std::memory_order_acquire), 1);

  tx->Disable();
  local_rx->Disable();
  remote_rx->Disable();
  CleanupChannel(channel);
}

}  // namespace
