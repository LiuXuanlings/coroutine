#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <unistd.h>

#include <google/protobuf/descriptor.pb.h>

#include "minicyber/node/node.h"
#include "minicyber/proto/role_attributes.pb.h"
#include "minicyber/topology/topology_manager.h"

namespace {

using TestMessage = minicyber::proto::RoleAttributes;
using minicyber::node::Node;
using minicyber::topology::TopologyManager;

TestMessage MakeMessage(const std::string& value) {
  TestMessage message;
  message.set_node_name(value);
  return message;
}

class NodeTest : public ::testing::Test {
 protected:
  void TearDown() override { TopologyManager::Instance()->Shutdown(); }
};

TEST_F(NodeTest, ConstructorRegistersToChannelManager) {
  Node node("test_node_1");
  EXPECT_TRUE(TopologyManager::Instance()->HasNode("test_node_1", ::getpid()));
}

TEST_F(NodeTest, WriterBuildsCompleteRoleAttributes) {
  Node node("role_writer_node");
  auto writer = node.CreateWriter<TestMessage>("/node/role_attributes");
  ASSERT_NE(writer, nullptr);

  const auto& attr = writer->role_attr();
  EXPECT_FALSE(attr.host_name().empty());
  EXPECT_EQ(attr.process_id(), ::getpid());
  EXPECT_EQ(attr.node_name(), "role_writer_node");
  EXPECT_EQ(attr.channel_name(), "/node/role_attributes");
  EXPECT_NE(attr.node_id(), 0u);
  EXPECT_NE(attr.channel_id(), 0u);
  EXPECT_NE(attr.id(), 0u);
  EXPECT_EQ(attr.message_type(), TestMessage::descriptor()->full_name());
  EXPECT_FALSE(attr.proto_desc().empty());
  google::protobuf::FileDescriptorProto descriptor;
  ASSERT_TRUE(descriptor.ParseFromString(attr.proto_desc()));
  EXPECT_EQ(descriptor.name(), TestMessage::descriptor()->file()->name());
}

TEST_F(NodeTest, DynamicJoinEnablesHybridAndHasReader) {
  const std::string channel = "/node/dynamic_join";
  std::atomic<int> callbacks{0};
  std::string received;
  Node subscriber("node_subscriber");
  auto reader = subscriber.CreateReader<TestMessage>(
      channel, [&](const std::shared_ptr<TestMessage>& message) {
        received = message->node_name();
        callbacks.fetch_add(1, std::memory_order_release);
      });
  ASSERT_NE(reader, nullptr);
  EXPECT_FALSE(reader->HasWriter());

  Node publisher("node_publisher");
  auto writer = publisher.CreateWriter<TestMessage>(channel);
  ASSERT_NE(writer, nullptr);
  EXPECT_TRUE(writer->HasReader());
  EXPECT_TRUE(reader->HasWriter());

  ASSERT_TRUE(writer->Write(MakeMessage("dynamic-delivery")));
  EXPECT_EQ(callbacks.load(std::memory_order_acquire), 1);
  EXPECT_EQ(received, "dynamic-delivery");

  reader->Shutdown();
  EXPECT_FALSE(writer->HasReader());
  ASSERT_TRUE(writer->Write(MakeMessage("after-reader-leave")));
  EXPECT_EQ(callbacks.load(std::memory_order_acquire), 1);
}

TEST_F(NodeTest, ReaderKeepsBoundedHistory) {
  const std::string channel = "/node/history";
  Node subscriber("history_subscriber");
  auto reader = subscriber.CreateReader<TestMessage>(channel, nullptr, 2);
  ASSERT_NE(reader, nullptr);
  Node publisher("history_publisher");
  auto writer = publisher.CreateWriter<TestMessage>(channel);
  ASSERT_NE(writer, nullptr);

  ASSERT_TRUE(writer->Write(MakeMessage("first")));
  ASSERT_TRUE(writer->Write(MakeMessage("second")));
  ASSERT_TRUE(writer->Write(MakeMessage("third")));
  EXPECT_TRUE(reader->HasReceived());
  EXPECT_EQ(reader->PendingQueueSize(), 2u);

  reader->Observe();
  ASSERT_NE(reader->GetOldestObserved(), nullptr);
  ASSERT_NE(reader->GetLatestObserved(), nullptr);
  EXPECT_EQ(reader->GetOldestObserved()->node_name(), "second");
  EXPECT_EQ(reader->GetLatestObserved()->node_name(), "third");
  reader->ClearData();
  EXPECT_TRUE(reader->Empty());
  EXPECT_FALSE(reader->HasReceived());
}

TEST_F(NodeTest, EndpointInitializationAndShutdownAreIdempotent) {
  const std::string channel = "/node/idem";
  Node node("idem_node");
  auto writer = node.CreateWriter<TestMessage>(channel);
  auto reader = node.CreateReader<TestMessage>(
      "/node/idem_reader", [](const std::shared_ptr<TestMessage>&) {});
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(writer->Init());
  EXPECT_TRUE(reader->Init());

  writer->Shutdown();
  writer->Shutdown();
  reader->Shutdown();
  reader->Shutdown();
  EXPECT_FALSE(writer->IsInit());
  EXPECT_FALSE(reader->IsInit());
  EXPECT_FALSE(writer->Write(MakeMessage("after-shutdown")));
}

TEST_F(NodeTest, NodeRejectsEmptyAndDuplicateReaderChannels) {
  Node node("node_rejects_invalid_channels");
  EXPECT_EQ(node.CreateWriter<TestMessage>(""), nullptr);
  EXPECT_EQ(node.CreateReader<TestMessage>(
                "", [](const std::shared_ptr<TestMessage>&) {}),
            nullptr);

  auto first = node.CreateReader<TestMessage>(
      "/node/duplicate", [](const std::shared_ptr<TestMessage>&) {});
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(node.CreateReader<TestMessage>(
                "/node/duplicate", [](const std::shared_ptr<TestMessage>&) {}),
            nullptr);
}

TEST_F(NodeTest, NodeShutdownDisablesRetainedEndpoints) {
  Node node("node_shutdown");
  auto writer = node.CreateWriter<TestMessage>("/node/shutdown_writer");
  auto reader = node.CreateReader<TestMessage>(
      "/node/shutdown_reader", [](const std::shared_ptr<TestMessage>&) {});
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  node.Shutdown();
  EXPECT_TRUE(node.IsShutdown());
  EXPECT_FALSE(writer->IsInit());
  EXPECT_FALSE(reader->IsInit());
  EXPECT_FALSE(writer->Write(MakeMessage("after-node-shutdown")));
  EXPECT_EQ(node.CreateWriter<TestMessage>("/node/after_shutdown"), nullptr);
}

}  // namespace
