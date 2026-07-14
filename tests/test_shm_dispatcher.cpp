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
#include <vector>

#include "minicyber/data/channel_buffer.h"
#include "minicyber/data/data_dispatcher.h"
#include "minicyber/data/data_notifier.h"
#include "minicyber/transport/dispatcher/shm_dispatcher.h"
#include "minicyber/transport/shm/condition_notifier.h"
#include "minicyber/transport/shm/posix_segment.h"

using minicyber::data::ChannelBuffer;
using minicyber::data::DataDispatcher;
using minicyber::data::DataNotifier;
using minicyber::data::Notifier;
using minicyber::transport::ConditionNotifier;
using minicyber::transport::PosixSegment;
using minicyber::transport::ReadableInfo;
using minicyber::transport::ShmDispatcher;
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

// 写一条消息到 PosixSegment 并通过 ConditionNotifier 通知
void WriteAndNotify(PosixSegment& seg, ConditionNotifier& n,
                   uint64_t channel_id, const std::string& payload) {
  ShmWritableBlock wb;
  if (!seg.AcquireBlockToWrite(payload.size(), &wb)) return;
  std::memcpy(wb.buf, payload.data(), payload.size());
  wb.block->set_msg_size(payload.size());
  uint32_t block_index = wb.index;
  seg.ReleaseWrittenBlock(wb);
  ReadableInfo info{0, block_index, channel_id};
  n.Notify(info);
}
}  // namespace

// 全局测试环境：在所有测试开始前清理可能残留的 notifier SHM。
// ::testing::Environment 是 Google Test 框架的**全局测试环境基类**
// :: 是全局命名空间作用域符，显式访问 gtest 的 testing 命名空间下的 Environment 类
// 继承该类可自定义全局钩子：SetUp 在所有测试用例启动前执行1次，TearDown 在所有测试结束后执行1次
// 注意：不能在单个测试中清理——ShmDispatcher 是单例，其 ConditionNotifier
// 在第一次访问时初始化并 shmat 到 SHM 段。如果在测试中调用 IPC_RMID，
// 写者的 Init() 会创建新段，而单例仍挂在旧段上，两者断联导致测试失败。
class ShmDispatcherTestEnv : public ::testing::Environment {
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

::testing::Environment* g_env = ::testing::AddGlobalTestEnvironment(new ShmDispatcherTestEnv);

// 单例可获取且 Init 启动后台线程
TEST(ShmDispatcherTest, SingletonAndRunning) {
  auto* d = ShmDispatcher::Instance();
  ASSERT_NE(d, nullptr);
  EXPECT_TRUE(d->IsRunning());
}

// Instance 多次获取同一对象
TEST(ShmDispatcherTest, SingletonIdentity) {
  auto* a = ShmDispatcher::Instance();
  auto* b = ShmDispatcher::Instance();
  EXPECT_EQ(a, b);
}

// AddSegment 打开 PosixSegment，/dev/shm 文件出现
TEST(ShmDispatcherTest, AddSegmentOpensShm) {
  const uint64_t CH = 86001;
  UnlinkShm("minicyber_" + std::to_string(CH));
  auto* d = ShmDispatcher::Instance();
  d->AddSegment(CH);
  EXPECT_TRUE(ShmFileExists("minicyber_" + std::to_string(CH)));
}

// 同进程：写 SHM + Notify，ShmDispatcher 后台线程读出并注入 DataDispatcher
TEST(ShmDispatcherTest, WriteNotifyThenReceive) {
  const uint64_t CH = 86002;
  UnlinkShm("minicyber_" + std::to_string(CH));

  PosixSegment seg(CH, 1024, 4);
  ASSERT_TRUE(seg.Open());

  using BT = ChannelBuffer<std::string>::BufferType;
  auto buf = std::make_shared<BT>(10);
  ChannelBuffer<std::string> cb(CH, buf);
  DataDispatcher<std::string>::Instance()->AddBuffer(cb);

  std::atomic<int> fired{0};
  auto notifier = std::make_shared<Notifier>();
  notifier->callback = [&]() { ++fired; };
  DataNotifier::Instance()->AddNotifier(CH, notifier);

  auto* d = ShmDispatcher::Instance();
  d->AddSegment(CH);

  // 写者用自己的 ConditionNotifier（同 key_t SysV SHM，与 ShmDispatcher 共享）
  ConditionNotifier writer_notifier;
  ASSERT_TRUE(writer_notifier.Init());

  WriteAndNotify(seg, writer_notifier, CH, "hello");

  std::shared_ptr<std::string> got;
  for (int i = 0; i < 100; ++i) {
    uint64_t idx = 0;
    if (cb.Fetch(&idx, got)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(got != nullptr);
  EXPECT_EQ(*got, "hello");
  EXPECT_GE(fired.load(), 1);

  writer_notifier.Shutdown();
  seg.Destroy();
  UnlinkShm("minicyber_" + std::to_string(CH));
}

// 多条消息顺序注入
TEST(ShmDispatcherTest, MultipleMessages) {
  const uint64_t CH = 86003;
  UnlinkShm("minicyber_" + std::to_string(CH));

  PosixSegment seg(CH, 1024, 4);
  ASSERT_TRUE(seg.Open());

  using BT = ChannelBuffer<std::string>::BufferType;
  auto buf = std::make_shared<BT>(100);
  ChannelBuffer<std::string> cb(CH, buf);
  DataDispatcher<std::string>::Instance()->AddBuffer(cb);

  auto* d = ShmDispatcher::Instance();
  d->AddSegment(CH);

  ConditionNotifier writer_notifier;
  ASSERT_TRUE(writer_notifier.Init());

  for (int i = 0; i < 3; ++i) {
    WriteAndNotify(seg, writer_notifier, CH, std::to_string(i));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // 等待后台读取
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::vector<std::shared_ptr<std::string>> vec;
  cb.FetchMulti(10, &vec);
  EXPECT_GE(vec.size(), 1u);

  writer_notifier.Shutdown();
  seg.Destroy();
  UnlinkShm("minicyber_" + std::to_string(CH));
}

// AddSegment 幂等：重复调用不替换已有 segment
TEST(ShmDispatcherTest, AddSegmentIdempotent) {
  const uint64_t CH = 86004;
  UnlinkShm("minicyber_" + std::to_string(CH));
  auto* d = ShmDispatcher::Instance();
  d->AddSegment(CH);
  ASSERT_TRUE(ShmFileExists("minicyber_" + std::to_string(CH)));
  d->AddSegment(CH);
  EXPECT_TRUE(ShmFileExists("minicyber_" + std::to_string(CH)));
}

// 真正的跨进程测试：fork 子进程写 SHM + Notify，
// 父进程的 ShmDispatcher 读出并注入 DataDispatcher
// （原生 SysV SHM 方案支持任意进程独立 Init 共享同 key_t，不限于 fork）
TEST(ShmDispatcherTest, ForkCrossProcessWriteNotify) {
  const uint64_t CH = 86005;
  UnlinkShm("minicyber_" + std::to_string(CH));

  // 父进程先创建 SHM 段
  PosixSegment seg(CH, 1024, 4);
  ASSERT_TRUE(seg.Open());

  // 父进程注册订阅
  using BT = ChannelBuffer<std::string>::BufferType;
  auto buf = std::make_shared<BT>(10);
  ChannelBuffer<std::string> cb(CH, buf);
  DataDispatcher<std::string>::Instance()->AddBuffer(cb);

  auto* d = ShmDispatcher::Instance();
  d->AddSegment(CH);

  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    // 子进程：独立打开同一段 SHM + 独立 ConditionNotifier（同 key_t）
    PosixSegment child_seg(CH, 1024, 4);
    if (!child_seg.Open()) _exit(10);
    ConditionNotifier child_notifier;
    if (!child_notifier.Init()) _exit(11);
    // 稍等父进程就绪
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    WriteAndNotify(child_seg, child_notifier, CH, "from-child");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    child_notifier.Shutdown();
    child_seg.Close();  // 不 Destroy，避免影响父进程
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

  seg.Destroy();
  UnlinkShm("minicyber_" + std::to_string(CH));
}