#include <sys/wait.h>
#include <unistd.h>

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
  for (int attempt = 0; attempt < 40; ++attempt) {
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
  ASSERT_EQ(pipe(writer_ready), 0);
  ASSERT_EQ(pipe(reader_ready), 0);
  ASSERT_EQ(pipe(writer_go), 0);
  ASSERT_EQ(pipe(reader_go), 0);

  const std::string channel_name = "/minicyber/mc606/discovery";
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
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    if (!topology->Leave(minicyber::proto::ROLE_WRITER,
                         MakeRole("writer", channel_name))) {
      _exit(6);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    topology->Shutdown();
    _exit(0);
  }

  const pid_t reader = fork();
  ASSERT_NE(reader, -1);
  if (reader == 0) {
    auto* topology = minicyber::topology::TopologyManager::Instance();
    if (!topology->Start()) _exit(7);
    const char ready = 1;
    if (write(reader_ready[1], &ready, sizeof(ready)) != sizeof(ready)) _exit(8);
    char go = 0;
    if (read(reader_go[0], &go, sizeof(go)) != sizeof(go)) _exit(9);
    if (!topology->Join(minicyber::proto::ROLE_READER,
                        MakeRole("reader", channel_name))) {
      _exit(10);
    }
    const bool joined = WaitFor([&] { return topology->HasWriter(channel_name); });
    const bool left = joined && WaitFor([&] { return !topology->HasWriter(channel_name); });
    topology->Shutdown();
    _exit(left ? 0 : 11);
  }

  close(writer_ready[1]);
  close(reader_ready[1]);
  close(writer_go[0]);
  close(reader_go[0]);
  ReadByte(writer_ready[0]);
  ReadByte(reader_ready[0]);
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  WriteByte(writer_go[1]);
  WriteByte(reader_go[1]);

  int writer_status = 0;
  int reader_status = 0;
  ASSERT_EQ(waitpid(writer, &writer_status, 0), writer);
  ASSERT_EQ(waitpid(reader, &reader_status, 0), reader);
  ASSERT_TRUE(WIFEXITED(writer_status));
  ASSERT_TRUE(WIFEXITED(reader_status));
  EXPECT_EQ(WEXITSTATUS(writer_status), 0);
  EXPECT_EQ(WEXITSTATUS(reader_status), 0);
}
