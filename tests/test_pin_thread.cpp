#include <gtest/gtest.h>
#include "minicyber/scheduler/common/pin_thread.h"

#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <atomic>
#include <thread>
#include <vector>

namespace minicyber {
namespace scheduler {

// ----------------------------------------------------------------------
// ParseCpuset 测试
// ----------------------------------------------------------------------
TEST(PinThreadTest, ParseCpusetRangeAndSingle) {
  // "0-1,2-3" -> 4 个 CPU
  std::vector<int> cpus;
  ParseCpuset("0-1,2-3", &cpus);
  ASSERT_EQ(cpus.size(), 4u);
  EXPECT_EQ(cpus[0], 0);
  EXPECT_EQ(cpus[1], 1);
  EXPECT_EQ(cpus[2], 2);
  EXPECT_EQ(cpus[3], 3);
}

TEST(PinThreadTest, ParseCpusetSingleRange) {
  std::vector<int> cpus;
  ParseCpuset("0-1", &cpus);
  ASSERT_EQ(cpus.size(), 2u);
  EXPECT_EQ(cpus[0], 0);
  EXPECT_EQ(cpus[1], 1);
}

TEST(PinThreadTest, ParseCpusetSingleCpu) {
  std::vector<int> cpus;
  ParseCpuset("0", &cpus);
  ASSERT_EQ(cpus.size(), 1u);
  EXPECT_EQ(cpus[0], 0);
}

TEST(PinThreadTest, ParseCpusetInvalidThrows) {
  std::vector<int> cpus;
  // "0-1-2" 有两个 '-'，range.size() == 3，应抛异常
  EXPECT_THROW(ParseCpuset("0-1-2", &cpus), std::invalid_argument);
}

// ----------------------------------------------------------------------
// SetSchedAffinity 测试
// ----------------------------------------------------------------------

TEST(PinThreadTest, SetSchedAffinityRange) {
  std::vector<int> cpus = {0, 1};
  std::thread t([]() {});

  SetSchedAffinity(&t, cpus, "range");

  // 验证线程的亲和性确实被设置了
  cpu_set_t get_set;
  CPU_ZERO(&get_set);
  pthread_getaffinity_np(t.native_handle(), sizeof(get_set), &get_set);

  // 应该只有 cpus 中的 CPU 被设置
  EXPECT_TRUE(CPU_ISSET(0, &get_set));
  EXPECT_TRUE(CPU_ISSET(1, &get_set));
  // 不应该有 cpus 之外的 CPU（在 4 核机器上检查 CPU 2,3）
  if (cpus.size() < static_cast<size_t>(CPU_SETSIZE)) {
    EXPECT_FALSE(CPU_ISSET(2, &get_set));
  }

  if (t.joinable()) t.join();
}

TEST(PinThreadTest, SetSchedAffinity1to1) {
  std::vector<int> cpus = {0, 1};
  std::thread t([]() {});

  // 1to1 模式，cpu_id=1 表示绑定到 cpus[1] = 1
  SetSchedAffinity(&t, cpus, "1to1", 1);

  cpu_set_t get_set;
  CPU_ZERO(&get_set);
  pthread_getaffinity_np(t.native_handle(), sizeof(get_set), &get_set);

  // 应该只有 CPU 1 被设置
  EXPECT_TRUE(CPU_ISSET(1, &get_set));
  EXPECT_FALSE(CPU_ISSET(0, &get_set));

  if (t.joinable()) t.join();
}

TEST(PinThreadTest, SetSchedAffinityEmptyCpusNoop) {
  std::vector<int> cpus;  // 空
  std::thread t([]() {});

  // 空 cpus 应直接返回，不崩溃
  SetSchedAffinity(&t, cpus, "range");

  if (t.joinable()) t.join();
}

TEST(PinThreadTest, SetSchedAffinity1to1InvalidCpuId) {
  std::vector<int> cpus = {0, 1};
  std::thread t([]() {});

  // cpu_id 越界应直接返回，不崩溃
  SetSchedAffinity(&t, cpus, "1to1", 99);
  SetSchedAffinity(&t, cpus, "1to1", -1);

  if (t.joinable()) t.join();
}

// ----------------------------------------------------------------------
// SetSchedPolicy 测试
// ----------------------------------------------------------------------
// 注意：SCHED_FIFO/SCHED_RR 需要 CAP_SYS_NICE 权限才能设置实时策略。
// 普通用户调用 pthread_setschedparam 会返回 EPERM，这是预期行为。
// 测试只验证调用不崩溃、线程能正常 join，不强制要求策略设置成功。

TEST(PinThreadTest, SetSchedPolicyFifo) {
  std::thread t([]() {});
  // 实时策略调用，无 root 权限会 EPERM，但不影响线程 join
  SetSchedPolicy(&t, "SCHED_FIFO", 10);
  if (t.joinable()) t.join();
}

TEST(PinThreadTest, SetSchedPolicyRr) {
  std::thread t([]() {});
  SetSchedPolicy(&t, "SCHED_RR", 10);
  if (t.joinable()) t.join();
}

TEST(PinThreadTest, SetSchedPolicyOther) {
  // SCHED_OTHER 通过 nice 值调整优先级，普通用户可设置
  std::atomic<pid_t> tid{-1};
  std::thread t([&]() {
    tid = static_cast<pid_t>(syscall(SYS_gettid));
  });
  // 等待线程拿到自己的 TID
  while (tid.load() == -1) {
    std::this_thread::yield();
  }

  // 设置 nice 值为 5
  SetSchedPolicy(&t, "SCHED_OTHER", 5, tid.load());

  if (t.joinable()) t.join();
}

TEST(PinThreadTest, SetSchedPolicyUnknownNoop) {
  // 未知策略应直接忽略，不崩溃
  std::thread t([]() {});
  SetSchedPolicy(&t, "SCHED_UNKNOWN", 0);
  if (t.joinable()) t.join();
}

}  // namespace scheduler
}  // namespace minicyber
