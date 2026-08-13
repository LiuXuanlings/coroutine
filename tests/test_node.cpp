#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <string>

#include "minicyber/node/node.h"
#include "minicyber/node/reader.h"
#include "minicyber/node/writer.h"
#include "minicyber/topology/topology_manager.h"

using minicyber::node::Node;
using minicyber::node::Reader;
using minicyber::node::Writer;
using minicyber::topology::TopologyManager;

namespace {
const pid_t kPid = ::getpid();
}  // namespace

// Node 构造时自动注册到 TopologyManager
TEST(NodeTest, ConstructorRegistersToTopology) {
  Node node("test_node_1");
  EXPECT_TRUE(TopologyManager::Instance()->HasNode("test_node_1", kPid));
}

// CreateWriter 返回有效 Writer，Write 成功
TEST(NodeTest, CreateWriterAndWrite) {
  Node node("test_node_2");
  auto w = node.CreateWriter<std::string>("/ch/write_test");
  ASSERT_NE(w, nullptr);
  EXPECT_TRUE(w->Write(std::make_shared<std::string>("hello")));
}

// CreateReader 返回有效 Reader
TEST(NodeTest, CreateReader) {
  Node node("test_node_3");
  auto r = node.CreateReader<std::string>(
      "/ch/read_test", [](const std::shared_ptr<std::string>&) {});
  ASSERT_NE(r, nullptr);
}

// 端到端：先注册拓扑（同 pid），再 CreateWriter + CreateReader，Write -> 回调触发
TEST(NodeTest, EndToEndSameProc) {
  const std::string CH = "/ch/node_e2e";
  // 预注册拓扑：writer 和 reader 同 pid -> IsSameProc true -> INTRA
  auto* topo = TopologyManager::Instance();
  topo->AddChannelWriter(CH, "e2e_writer_node", kPid);
  topo->AddChannelReader(CH, "e2e_reader_node", kPid);

  std::atomic<int> count{0};
  std::string received;
  std::mutex mu;

  Node reader_node("e2e_reader_node");
  auto r = reader_node.CreateReader<std::string>(
      CH, [&](const std::shared_ptr<std::string>& msg) {
        ++count;
        std::lock_guard<std::mutex> lg(mu);
        received = *msg;
      });

  Node writer_node("e2e_writer_node");
  auto w = writer_node.CreateWriter<std::string>(CH);

  ASSERT_TRUE(w->Write(std::make_shared<std::string>("e2e-payload")));
  EXPECT_GE(count.load(), 1);
  EXPECT_EQ(received, "e2e-payload");
}

// Write before Init returns false
TEST(NodeTest, WriteBeforeInitReturnsFalse) {
  // 构造一个 Writer 但不调 Init（通过 Node 内部已 Init，这里测 Shutdown 后）
  Node node("test_node_5");
  auto w = node.CreateWriter<std::string>("/ch/shutdown_test");
  ASSERT_NE(w, nullptr);
  ASSERT_TRUE(w->Write(std::make_shared<std::string>("before")));
  w->Shutdown();
  EXPECT_FALSE(w->Write(std::make_shared<std::string>("after")));
}

// Reader Shutdown 幂等
TEST(NodeTest, ReaderShutdownIdempotent) {
  Node node("test_node_6");
  auto r = node.CreateReader<std::string>(
      "/ch/reader_shutdown", [](const std::shared_ptr<std::string>&) {});
  r->Shutdown();
  r->Shutdown();  // 不崩溃
}

// Writer Shutdown 幂等
TEST(NodeTest, WriterShutdownIdempotent) {
  Node node("test_node_7");
  auto w = node.CreateWriter<std::string>("/ch/writer_shutdown");
  w->Shutdown();
  w->Shutdown();  // 不崩溃
}

// Node 跟踪多个 Reader，GetReader 返回正确的
TEST(NodeTest, GetReaderReturnsCorrect) {
  Node node("test_node_8");
  auto r1 = node.CreateReader<std::string>(
      "/ch/get_r1", [](const std::shared_ptr<std::string>&) {});
  auto r2 = node.CreateReader<std::string>(
      "/ch/get_r2", [](const std::shared_ptr<std::string>&) {});
  ASSERT_NE(r1, nullptr);
  ASSERT_NE(r2, nullptr);
  auto got1 = node.GetReader<std::string>("/ch/get_r1");
  auto got2 = node.GetReader<std::string>("/ch/get_r2");
  EXPECT_EQ(got1.get(), r1.get());
  EXPECT_EQ(got2.get(), r2.get());
  EXPECT_NE(got1.get(), got2.get());
}

// Node name 可获取
TEST(NodeTest, NodeNameAccessible) {
  Node node("my_named_node");
  EXPECT_EQ(node.Name(), "my_named_node");
}

// 多个 Writer 在不同 channel 上隔离
TEST(NodeTest, MultipleWritersIsolated) {
  Node node("test_node_10");
  auto w1 = node.CreateWriter<std::string>("/ch/iso_w1");
  auto w2 = node.CreateWriter<std::string>("/ch/iso_w2");
  ASSERT_NE(w1, nullptr);
  ASSERT_NE(w2, nullptr);
  EXPECT_TRUE(w1->Write(std::make_shared<std::string>("w1-msg")));
  EXPECT_TRUE(w2->Write(std::make_shared<std::string>("w2-msg")));
}

// Node Shutdown 应关闭已创建端点，即使调用方仍持有对应 shared_ptr。
TEST(NodeTest, ShutdownDisablesRetainedEndpoints) {
  Node node("test_node_shutdown");
  auto writer = node.CreateWriter<std::string>("/ch/node_shutdown");
  auto reader = node.CreateReader<std::string>(
      "/ch/node_shutdown_reader", [](const std::shared_ptr<std::string>&) {});
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  node.Shutdown();

  EXPECT_TRUE(node.IsShutdown());
  EXPECT_FALSE(writer->Write(std::make_shared<std::string>("after-shutdown")));
  EXPECT_FALSE(reader->IsInit());
  EXPECT_EQ(node.CreateWriter<std::string>("/ch/after_shutdown"), nullptr);
  EXPECT_EQ(node.CreateReader<std::string>(
                "/ch/after_shutdown_reader",
                [](const std::shared_ptr<std::string>&) {}),
            nullptr);
  node.Shutdown();
}

// NodeChannelImpl 必须拒绝空 channel，避免向拓扑注册无效端点。
TEST(NodeTest, EmptyChannelIsRejected) {
  Node node("test_node_empty_channel");
  EXPECT_EQ(node.CreateWriter<std::string>(""), nullptr);
  EXPECT_EQ(node.CreateReader<std::string>(
                "", [](const std::shared_ptr<std::string>&) {}),
            nullptr);
}

TEST(NodeTest, ReaderKeepsBoundedHistoryAndInvokesCallback) {
  const std::string channel = "/ch/node_reader_history";
  Node subscriber("test_node_history_subscriber");
  std::atomic<int> callbacks{0};
  auto reader = subscriber.CreateReader<std::string>(
      channel, [&](const std::shared_ptr<std::string>&) { ++callbacks; }, 2);
  Node publisher("test_node_history_publisher");
  auto writer = publisher.CreateWriter<std::string>(channel);
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(writer, nullptr);

  ASSERT_TRUE(writer->Write("first"));
  ASSERT_TRUE(writer->Write("second"));
  ASSERT_TRUE(writer->Write("third"));
  EXPECT_EQ(callbacks.load(), 3);
  EXPECT_TRUE(reader->HasReceived());
  EXPECT_EQ(reader->PendingQueueSize(), 2u);

  reader->Observe();
  EXPECT_FALSE(reader->Empty());
  ASSERT_NE(reader->GetOldestObserved(), nullptr);
  ASSERT_NE(reader->GetLatestObserved(), nullptr);
  EXPECT_EQ(*reader->GetOldestObserved(), "second");
  EXPECT_EQ(*reader->GetLatestObserved(), "third");
  reader->ClearData();
  EXPECT_TRUE(reader->Empty());
}

TEST(NodeTest, WriterRejectsNullMessage) {
  Node node("test_node_null_message");
  auto writer = node.CreateWriter<std::string>("/ch/null_message");
  ASSERT_NE(writer, nullptr);
  EXPECT_FALSE(writer->Write(std::shared_ptr<std::string>()));
}
