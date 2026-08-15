#include <gtest/gtest.h>
#include "minicyber/scheduler/common/pin_thread.h"

#include <algorithm>
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
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  ASSERT_EQ(0, sched_getaffinity(0, sizeof(allowed), &allowed));
  std::vector<int> cpus;
  for (int cpu = 0; cpu < CPU_SETSIZE && cpus.size() < 2; ++cpu) {
    if (CPU_ISSET(cpu, &allowed)) cpus.push_back(cpu);
  }
  ASSERT_FALSE(cpus.empty());
  std::atomic<bool> stop{false};
  std::thread t([&]() {
    while (!stop.load(std::memory_order_acquire)) std::this_thread::yield();
  });

  EXPECT_TRUE(SetSchedAffinity(&t, cpus, "range"));

  // 验证线程的亲和性确实被设置了
  cpu_set_t get_set;
  CPU_ZERO(&get_set);
  EXPECT_EQ(0, pthread_getaffinity_np(t.native_handle(), sizeof(get_set),
                                     &get_set));

  // 应该只有 cpus 中的 CPU 被设置
  for (const int cpu : cpus) {
    EXPECT_TRUE(CPU_ISSET(cpu, &get_set));
  }
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (std::find(cpus.begin(), cpus.end(), cpu) == cpus.end()) {
      EXPECT_FALSE(CPU_ISSET(cpu, &get_set));
    }
  }

  stop.store(true, std::memory_order_release);
  if (t.joinable()) t.join();
}

TEST(PinThreadTest, SetSchedAffinity1to1) {
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  ASSERT_EQ(0, sched_getaffinity(0, sizeof(allowed), &allowed));
  std::vector<int> cpus;
  for (int cpu = 0; cpu < CPU_SETSIZE && cpus.size() < 2; ++cpu) {
    if (CPU_ISSET(cpu, &allowed)) cpus.push_back(cpu);
  }
  ASSERT_FALSE(cpus.empty());
  const int target_index = cpus.size() > 1 ? 1 : 0;
  std::atomic<bool> stop{false};
  std::thread t([&]() {
    while (!stop.load(std::memory_order_acquire)) std::this_thread::yield();
  });

  EXPECT_TRUE(SetSchedAffinity(&t, cpus, "1to1", target_index));

  cpu_set_t get_set;
  CPU_ZERO(&get_set);
  EXPECT_EQ(0, pthread_getaffinity_np(t.native_handle(), sizeof(get_set),
                                     &get_set));

  EXPECT_TRUE(CPU_ISSET(cpus[target_index], &get_set));
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (cpu != cpus[target_index]) {
      EXPECT_FALSE(CPU_ISSET(cpu, &get_set));
    }
  }

  stop.store(true, std::memory_order_release);
  if (t.joinable()) t.join();
}

TEST(PinThreadTest, SetSchedAffinityEmptyCpusNoop) {
  std::vector<int> cpus;  // 空
  std::thread t([]() {});

  // 空 cpus 应直接返回，不崩溃
  EXPECT_TRUE(SetSchedAffinity(&t, cpus, "range"));

  if (t.joinable()) t.join();
}

TEST(PinThreadTest, SetSchedAffinity1to1InvalidCpuId) {
  std::vector<int> cpus = {0, 1};
  std::thread t([]() {});

  // cpu_id 越界应直接返回，不崩溃
  EXPECT_FALSE(SetSchedAffinity(&t, cpus, "1to1", 99));
  EXPECT_FALSE(SetSchedAffinity(&t, cpus, "1to1", -1));

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
