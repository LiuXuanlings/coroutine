#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "minicyber/topology/topology_manager.h"

namespace {

std::string HostName() {
  char name[256] = {};
  EXPECT_EQ(gethostname(name, sizeof(name) - 1), 0);
  return name;
}

minicyber::proto::RoleAttributes MakeRole(const std::string& node_name,
                                          const std::string& channel_name) {
  minicyber::proto::RoleAttributes attr;
  attr.set_host_name(HostName());
  attr.set_process_id(getpid());
  attr.set_node_name(node_name);
  attr.set_channel_name(channel_name);
  attr.set_channel_id(std::hash<std::string>{}(channel_name));
  attr.set_id(std::hash<std::string>{}(node_name) ^ static_cast<uint64_t>(getpid()));
  return attr;
}

bool WaitFor(const std::function<bool()>& predicate) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

void WriteByte(int fd) {
  const char value = 1;
  ASSERT_EQ(write(fd, &value, sizeof(value)), static_cast<ssize_t>(sizeof(value)));
}

void ReadByte(int fd) {
  char value = 0;
  ASSERT_EQ(read(fd, &value, sizeof(value)), static_cast<ssize_t>(sizeof(value)));
}

}  // namespace

TEST(TopologyDiscoveryTest, PropagatesSameHostJoinAndLeaveAcrossProcesses) {
  int writer_ready[2];
  int reader_ready[2];
  int writer_go[2];
  int reader_go[2];
  int reader_joined_ready[2];
  int reader_saw_writer[2];
  int reader_left_ack[2];
  ASSERT_EQ(pipe(writer_ready), 0);
  ASSERT_EQ(pipe(reader_ready), 0);
  ASSERT_EQ(pipe(writer_go), 0);
  ASSERT_EQ(pipe(reader_go), 0);
  ASSERT_EQ(pipe(reader_joined_ready), 0);
  ASSERT_EQ(pipe(reader_saw_writer), 0);
  ASSERT_EQ(pipe(reader_left_ack), 0);

  static std::atomic<uint64_t> run_id{0};
  const std::string channel_name = "/minicyber/mc606/discovery/" +
                                   std::to_string(getpid()) + "/" +
                                   std::to_string(run_id.fetch_add(1));
  const pid_t writer = fork();
  ASSERT_NE(writer, -1);
  if (writer == 0) {
    auto* topology = minicyber::topology::TopologyManager::Instance();
    if (!topology->Start()) _exit(2);
    const char ready = 1;
    if (write(writer_ready[1], &ready, sizeof(ready)) != sizeof(ready)) _exit(3);
    char go = 0;
    if (read(writer_go[0], &go, sizeof(go)) != sizeof(go)) _exit(4);
    if (!topology->Join(minicyber::proto::ROLE_WRITER,
                        MakeRole("writer", channel_name))) {
      _exit(5);
    }
    const bool reader_joined = WaitFor([&] { return topology->HasReader(channel_name); });
    if (!reader_joined ||
        topology->GetRelation(channel_name, getpid()) != minicyber::DIFF_PROC) {
      _exit(6);
    }
    char reader_observed_writer = 0;
    if (read(reader_saw_writer[0], &reader_observed_writer,
             sizeof(reader_observed_writer)) != sizeof(reader_observed_writer)) {
      _exit(19);
    }
    if (!topology->Leave(minicyber::proto::ROLE_WRITER,
                         MakeRole("writer", channel_name))) {
      _exit(12);
    }
    if (!WaitFor([&] { return !topology->HasReader(channel_name); })) {
      _exit(13);
    }
    const char ack = 1;
    if (write(reader_left_ack[1], &ack, sizeof(ack)) != sizeof(ack)) {
      _exit(15);
    }
    topology->Shutdown();
    _exit(0);
  }

  const pid_t reader = fork();
  ASSERT_NE(reader, -1);
  if (reader == 0) {
    auto* topology = minicyber::topology::TopologyManager::Instance();
    if (!topology->Start()) _exit(7);
    std::atomic<bool> writer_joined{false};
    std::atomic<bool> writer_left{false};
    const auto connection = topology->AddChangeListener(
        [&](const minicyber::proto::ChangeMsg& change) {
          if (change.role_type() != minicyber::proto::ROLE_WRITER ||
              change.role_attr().channel_name() != channel_name) {
            return;
          }
          if (change.operate_type() == minicyber::proto::OPT_JOIN) {
            writer_joined.store(true);
          } else if (change.operate_type() == minicyber::proto::OPT_LEAVE) {
            writer_left.store(true);
          }
        });
    const char ready = 1;
    if (write(reader_ready[1], &ready, sizeof(ready)) != sizeof(ready)) _exit(8);
    char go = 0;
    if (read(reader_go[0], &go, sizeof(go)) != sizeof(go)) _exit(9);
    if (!topology->Join(minicyber::proto::ROLE_READER,
                        MakeRole("reader", channel_name))) {
      _exit(10);
    }
    const char joined_ready = 1;
    if (write(reader_joined_ready[1], &joined_ready, sizeof(joined_ready)) !=
        sizeof(joined_ready)) {
      _exit(18);
    }
    const bool joined = WaitFor([&] { return writer_joined.load(); });
    if (!joined || !topology->HasWriter(channel_name) ||
        topology->GetRelation(channel_name, getpid()) != minicyber::DIFF_PROC) {
      topology->Shutdown();
      _exit(11);
    }
    const char saw_writer = 1;
    if (write(reader_saw_writer[1], &saw_writer, sizeof(saw_writer)) !=
        sizeof(saw_writer)) {
      _exit(20);
    }
    const bool left = WaitFor(
        [&] { return writer_left.load() && !topology->HasWriter(channel_name); });
    if (!topology->Leave(minicyber::proto::ROLE_READER,
                         MakeRole("reader", channel_name))) {
      _exit(14);
    }
    char ack = 0;
    if (read(reader_left_ack[0], &ack, sizeof(ack)) != sizeof(ack)) {
      _exit(16);
    }
    topology->RemoveChangeListener(connection);
    topology->Shutdown();
    _exit(left ? 0 : 17);
  }

  close(writer_ready[1]);
  close(reader_ready[1]);
  close(writer_go[0]);
  close(reader_go[0]);
  close(reader_joined_ready[1]);
  close(reader_saw_writer[0]);
  close(reader_saw_writer[1]);
  close(reader_left_ack[0]);
  close(reader_left_ack[1]);
  ReadByte(writer_ready[0]);
  ReadByte(reader_ready[0]);
  WriteByte(reader_go[1]);
  // Start 只表示本地 Participant 已创建；Reader 本地 Join 完成后才允许
  // Writer 发布。
  ReadByte(reader_joined_ready[0]);
  WriteByte(writer_go[1]);

  int writer_status = 0;
  int reader_status = 0;
  ASSERT_EQ(waitpid(writer, &writer_status, 0), writer);
  ASSERT_EQ(waitpid(reader, &reader_status, 0), reader);
  ASSERT_TRUE(WIFEXITED(writer_status));
  ASSERT_TRUE(WIFEXITED(reader_status));
  EXPECT_EQ(WEXITSTATUS(writer_status), 0);
  EXPECT_EQ(WEXITSTATUS(reader_status), 0);
}
