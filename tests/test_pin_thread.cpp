#include <gtest/gtest.h>
#include "minicyber/scheduler/common/pin_thread.h"

#include <pthread.h>
#include <sched.h>
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

}  // namespace scheduler
}  // namespace minicyber
