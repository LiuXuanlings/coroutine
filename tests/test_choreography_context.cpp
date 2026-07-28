#include <gtest/gtest.h>
#include "minicyber/scheduler/policy/choreography_context.h"
#include "minicyber/croutine/croutine.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace minicyber {
namespace scheduler {

// ----------------------------------------------------------------------
// 辅助：创建一个就绪协程
// ----------------------------------------------------------------------
static std::shared_ptr<CRoutine> MakeReadyCRoutine(uint32_t prio = 0,
                                                   uint64_t id = 0) {
  auto cr = std::make_shared<CRoutine>([]() {});
  cr->set_id(id);
  cr->set_priority(prio);
  cr->SetState(RoutineState::READY);
  return cr;
}

// ----------------------------------------------------------------------
// 测试 1：NextRoutine 空队列返回 nullptr
// ----------------------------------------------------------------------
TEST(ChoreographyContextTest, NextRoutineEmptyReturnsNull) {
  ChoreographyContext ctx;
  EXPECT_EQ(ctx.NextRoutine(), nullptr);
}

// ----------------------------------------------------------------------
// 测试 2：NextRoutine 按优先级从高到低返回
// ----------------------------------------------------------------------
TEST(ChoreographyContextTest, NextRoutineRespectsPriority) {
  ChoreographyContext ctx;

  auto cr_low = MakeReadyCRoutine(5, 1);
  auto cr_high = MakeReadyCRoutine(10, 2);
  ctx.Enqueue(cr_low);
  ctx.Enqueue(cr_high);

  // 第一个应返回高优先级
  auto first = ctx.NextRoutine();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->priority(), 10u);
  first->SetState(RoutineState::FINISHED);
  first->Release();

  // 第二个应返回低优先级
  auto second = ctx.NextRoutine();
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->priority(), 5u);
  second->Release();
}

// ----------------------------------------------------------------------
// 测试 3：NextRoutine 跳过非 READY 状态的协程
// ----------------------------------------------------------------------
TEST(ChoreographyContextTest, NextRoutineSkipsNonReady) {
  ChoreographyContext ctx;

  auto cr_wait = MakeReadyCRoutine(5, 1);
  cr_wait->SetState(RoutineState::DATA_WAIT);
  auto cr_ready = MakeReadyCRoutine(5, 2);

  ctx.Enqueue(cr_wait);
  ctx.Enqueue(cr_ready);

  // 应跳过 DATA_WAIT，返回 READY 的那个
  auto cr = ctx.NextRoutine();
  ASSERT_NE(cr, nullptr);
  EXPECT_EQ(cr->id(), 2u);
  cr->Release();
}

// ----------------------------------------------------------------------
// 测试 4：NextRoutine 返回的协程处于 Acquired 状态
// ----------------------------------------------------------------------
TEST(ChoreographyContextTest, NextRoutineAcquiresLock) {
  ChoreographyContext ctx;

  auto cr = MakeReadyCRoutine(0, 1);
  ctx.Enqueue(cr);

  auto out = ctx.NextRoutine();
  ASSERT_NE(out, nullptr);

  // 应已被 Acquire，再次 Acquire 失败
  EXPECT_FALSE(out->Acquire());
  out->Release();

  // Release 后可以再次 Acquire
  EXPECT_TRUE(out->Acquire());
  out->Release();
}

// ----------------------------------------------------------------------
// 测试 5：Enqueue 添加多个协程均可被 NextRoutine 取出
// ----------------------------------------------------------------------
TEST(ChoreographyContextTest, EnqueueAddsToQueue) {
  ChoreographyContext ctx;

  int count = 0;
  for (int i = 0; i < 5; ++i) {
    ctx.Enqueue(MakeReadyCRoutine(5, i + 1));
  }

  // 依次取出 5 个协程
  for (int i = 0; i < 5; ++i) {
    auto cr = ctx.NextRoutine();
    ASSERT_NE(cr, nullptr);
    cr->SetState(RoutineState::FINISHED);
    cr->Release();
    ++count;
  }
  EXPECT_EQ(count, 5);

  // 第 6 次应返回 nullptr（队列中协程均 FINISHED，无就绪）
  // 注意：FINISHED 协程仍在 cr_queue_ 中，但 NextRoutine 会跳过
  auto cr = ctx.NextRoutine();
  EXPECT_EQ(cr, nullptr);
}

// ----------------------------------------------------------------------
// 测试 6：Wait 阻塞直到 Notify 唤醒
// ----------------------------------------------------------------------
TEST(ChoreographyContextTest, WaitUnblocksOnNotify) {
  ChoreographyContext ctx;

  std::atomic<bool> wait_returned{false};
  std::thread waiter([&]() {
    ctx.Wait();
    wait_returned.store(true);
  });

  // 确保 waiter 进入 Wait
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(wait_returned.load());

  ctx.Notify();
  waiter.join();
  EXPECT_TRUE(wait_returned.load());
}

// ----------------------------------------------------------------------
// 测试 7：Shutdown 唤醒阻塞的 Wait
// ----------------------------------------------------------------------
TEST(ChoreographyContextTest, ShutdownUnblocksWait) {
  ChoreographyContext ctx;

  std::atomic<bool> wait_returned{false};
  std::thread waiter([&]() {
    ctx.Wait();
    wait_returned.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(wait_returned.load());

  ctx.Shutdown();
  waiter.join();
  EXPECT_TRUE(wait_returned.load());
}

// ----------------------------------------------------------------------
// 测试 8：RemoveCRoutine 按 id 移除
// ----------------------------------------------------------------------
TEST(ChoreographyContextTest, RemoveCRoutineById) {
  ChoreographyContext ctx;

  auto cr1 = MakeReadyCRoutine(5, 100);
  auto cr2 = MakeReadyCRoutine(5, 200);
  ctx.Enqueue(cr1);
  ctx.Enqueue(cr2);

  // 移除 cr1
  EXPECT_TRUE(ctx.RemoveCRoutine(100));

  // 队列中应只剩 cr2
  auto out = ctx.NextRoutine();
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->id(), 200u);
  out->SetState(RoutineState::FINISHED);
  out->Release();
}

// ----------------------------------------------------------------------
// 测试 9：RemoveCRoutine 不存在的 id 返回 false
// ----------------------------------------------------------------------
TEST(ChoreographyContextTest, RemoveCRoutineNotFound) {
  ChoreographyContext ctx;
  EXPECT_FALSE(ctx.RemoveCRoutine(999));
}

// ----------------------------------------------------------------------
// 测试 10：实例隔离——不同 ChoreographyContext 互不干扰
// ----------------------------------------------------------------------
// 关键差异验证：ChoreographyContext 无静态共享状态，A 的任务不会出现在 B。
// 这是与 ClassicContext（静态 group 共享）的核心区别。
// ----------------------------------------------------------------------
TEST(ChoreographyContextTest, NoStealingFromOtherContexts) {
  ChoreographyContext ctx_a;
  ChoreographyContext ctx_b;

  // 仅向 ctx_a 入队
  ctx_a.Enqueue(MakeReadyCRoutine(10, 1));

  // ctx_b 应返回 nullptr，不会"看到"ctx_a 的任务
  EXPECT_EQ(ctx_b.NextRoutine(), nullptr);

  // ctx_a 能正常取出
  auto cr = ctx_a.NextRoutine();
  ASSERT_NE(cr, nullptr);
  EXPECT_EQ(cr->id(), 1u);
  cr->Release();
}

}  // namespace scheduler
}  // namespace minicyber
