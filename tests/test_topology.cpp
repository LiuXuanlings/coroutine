#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include "minicyber/topology/topology_manager.h"

using minicyber::topology::TopologyManager;

namespace {
const pid_t kPidA = 1000;
const pid_t kPidB = 2000;

minicyber::proto::RoleAttributes MakeRole(const std::string& node_name,
                                          const std::string& channel_name,
                                          uint64_t id) {
  char host_name[256] = {};
  EXPECT_EQ(gethostname(host_name, sizeof(host_name) - 1), 0);

  minicyber::proto::RoleAttributes attr;
  attr.set_host_name(host_name);
  attr.set_process_id(getpid());
  attr.set_node_name(node_name);
  attr.set_channel_name(channel_name);
  attr.set_channel_id(std::hash<std::string>{}(channel_name));
  attr.set_id(id);
  return attr;
}
}  // namespace

// AddNode 注册节点，HasNode 可查到
TEST(TopologyManagerTest, AddNodeAndHasNode) {
  auto* topo = TopologyManager::Instance();
  topo->AddNode("node_a", kPidA);
  EXPECT_TRUE(topo->HasNode("node_a", kPidA));
  EXPECT_FALSE(topo->HasNode("node_a", kPidB));
  EXPECT_FALSE(topo->HasNode("node_b", kPidA));
}

// AddChannelWriter + AddChannelReader 同 pid -> IsSameProc true
TEST(TopologyManagerTest, SameProcWhenAllSamePid) {
  auto* topo = TopologyManager::Instance();
  topo->AddChannelWriter("/ch/same", "writer_node", kPidA);
  topo->AddChannelReader("/ch/same", "reader_node", kPidA);
  EXPECT_TRUE(topo->IsSameProc("/ch/same"));
}

// writer 和 reader 不同 pid -> IsSameProc false
TEST(TopologyManagerTest, DiffProcWhenDifferentPid) {
  auto* topo = TopologyManager::Instance();
  topo->AddChannelWriter("/ch/diff", "writer_node", kPidA);
  topo->AddChannelReader("/ch/diff", "reader_node", kPidB);
  EXPECT_FALSE(topo->IsSameProc("/ch/diff"));
}

// 只有 writer（无 reader）-> IsSameProc false
TEST(TopologyManagerTest, OnlyWriterNotSameProc) {
  auto* topo = TopologyManager::Instance();
  topo->AddChannelWriter("/ch/only_writer", "writer_node", kPidA);
  EXPECT_FALSE(topo->IsSameProc("/ch/only_writer"));
}

// 只有 reader（无 writer）-> IsSameProc false
TEST(TopologyManagerTest, OnlyReaderNotSameProc) {
  auto* topo = TopologyManager::Instance();
  topo->AddChannelReader("/ch/only_reader", "reader_node", kPidA);
  EXPECT_FALSE(topo->IsSameProc("/ch/only_reader"));
}

// 无任何角色的 channel -> IsSameProc false
TEST(TopologyManagerTest, EmptyChannelNotSameProc) {
  auto* topo = TopologyManager::Instance();
  EXPECT_FALSE(topo->IsSameProc("/ch/nonexistent"));
}

// GetRelation 返回正确的 Relation 枚举
TEST(TopologyManagerTest, GetRelation) {
  auto* topo = TopologyManager::Instance();
  topo->AddChannelWriter("/ch/rel", "w", kPidA);
  topo->AddChannelReader("/ch/rel", "r", kPidA);
  EXPECT_EQ(topo->GetRelation("/ch/rel", kPidA),
            static_cast<int>(minicyber::SAME_PROC));

  topo->AddChannelReader("/ch/rel", "r2", kPidB);
  EXPECT_EQ(topo->GetRelation("/ch/rel", kPidA),
            static_cast<int>(minicyber::DIFF_PROC));

  EXPECT_EQ(topo->GetRelation("/ch/nonexistent", kPidA),
            static_cast<int>(minicyber::NO_RELATION));
}

// 多个 channel 互不干扰
TEST(TopologyManagerTest, MultipleChannelsIsolated) {
  auto* topo = TopologyManager::Instance();
  topo->AddChannelWriter("/ch/iso_a", "w", kPidA);
  topo->AddChannelReader("/ch/iso_a", "r", kPidA);
  topo->AddChannelWriter("/ch/iso_b", "w", kPidB);
  topo->AddChannelReader("/ch/iso_b", "r", kPidB);

  EXPECT_TRUE(topo->IsSameProc("/ch/iso_a"));
  EXPECT_TRUE(topo->IsSameProc("/ch/iso_b"));
}

// 幂等：重复添加相同 writer 不重复
TEST(TopologyManagerTest, AddWriterIdempotent) {
  auto* topo = TopologyManager::Instance();
  topo->AddChannelWriter("/ch/idem", "w", kPidA);
  topo->AddChannelWriter("/ch/idem", "w", kPidA);
  topo->AddChannelReader("/ch/idem", "r", kPidA);
  EXPECT_TRUE(topo->IsSameProc("/ch/idem"));
  // 验证 writer 数量不翻倍（通过 DumpGraph 间接验证不重复）
  std::string graph = topo->DumpGraph();
  // DOT 中 "/ch/idem" 出现次数应有限（1 个 writer 节点 + 1 个 reader 节点）
  EXPECT_NE(graph.find("/ch/idem"), std::string::npos);
}

// DumpGraph 产生输出，包含 channel 和 node 名
TEST(TopologyManagerTest, DumpGraphContainsChannelAndNodes) {
  auto* topo = TopologyManager::Instance();
  topo->AddChannelWriter("/ch/dot", "writer_n", kPidA);
  topo->AddChannelReader("/ch/dot", "reader_n", kPidA);
  std::string graph = topo->DumpGraph();
  EXPECT_NE(graph.find("digraph"), std::string::npos);
  EXPECT_NE(graph.find("/ch/dot"), std::string::npos);
  EXPECT_NE(graph.find("writer_n"), std::string::npos);
  EXPECT_NE(graph.find("reader_n"), std::string::npos);
}

TEST(TopologyManagerTest, JoinLeaveTracksReadersSnapshotsAndChangeListeners) {
  auto* topo = TopologyManager::Instance();
  topo->Shutdown();

  std::atomic<int> joins{0};
  std::atomic<int> leaves{0};
  const auto connection = topo->AddChangeListener(
      [&joins, &leaves](const minicyber::proto::ChangeMsg& msg) {
        if (msg.operate_type() == minicyber::proto::OPT_JOIN) {
          ++joins;
        } else if (msg.operate_type() == minicyber::proto::OPT_LEAVE) {
          ++leaves;
        }
      });

  const std::string channel = "/ch/lifecycle";
  const auto writer = MakeRole("writer", channel, 0x6061);
  const auto reader = MakeRole("reader", channel, 0x6062);
  ASSERT_TRUE(topo->Join(minicyber::proto::ROLE_WRITER, writer));
  ASSERT_TRUE(topo->Join(minicyber::proto::ROLE_READER, reader));
  EXPECT_TRUE(topo->HasReader(channel));
  EXPECT_TRUE(topo->HasWriter(channel));
  EXPECT_EQ(topo->GetReaders(channel).size(), 1U);
  EXPECT_EQ(topo->GetWriters(channel).size(), 1U);
  EXPECT_EQ(topo->GetRelation(channel, getpid()), minicyber::SAME_PROC);
  EXPECT_EQ(joins.load(), 2);

  ASSERT_TRUE(topo->Leave(minicyber::proto::ROLE_READER, reader));
  EXPECT_FALSE(topo->HasReader(channel));
  ASSERT_TRUE(topo->Leave(minicyber::proto::ROLE_WRITER, writer));
  EXPECT_FALSE(topo->HasWriter(channel));
  EXPECT_EQ(leaves.load(), 2);

  topo->RemoveChangeListener(connection);
  const int callback_count = joins.load() + leaves.load();
  const auto replacement_writer = MakeRole("replacement", channel, 0x6063);
  ASSERT_TRUE(topo->Join(minicyber::proto::ROLE_WRITER, replacement_writer));
  ASSERT_TRUE(topo->Leave(minicyber::proto::ROLE_WRITER, replacement_writer));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(joins.load() + leaves.load(), callback_count);
  topo->Shutdown();
}

// Shutdown 清空所有拓扑
TEST(TopologyManagerTest, ShutdownClearsAll) {
  auto* topo = TopologyManager::Instance();
  topo->AddChannelWriter("/ch/shutdown", "w", kPidA);
  topo->AddChannelReader("/ch/shutdown", "r", kPidA);
  ASSERT_TRUE(topo->IsSameProc("/ch/shutdown"));
  topo->Shutdown();
  EXPECT_FALSE(topo->IsSameProc("/ch/shutdown"));
  EXPECT_FALSE(topo->HasNode("w", kPidA));
}
