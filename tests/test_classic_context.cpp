#include <gtest/gtest.h>
#include "minicyber/scheduler/policy/classic_context.h"
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
                                                    uint64_t id = 0,
                                                    const std::string& grp = DEFAULT_GROUP_NAME) {
  auto cr = std::make_shared<CRoutine>([]() {});
  cr->set_id(id);
  cr->set_priority(prio);
  cr->set_group_name(grp);
  cr->SetState(RoutineState::READY);
  return cr;
}

// ----------------------------------------------------------------------
// 测试 1：NextRoutine 空队列返回 nullptr
// ----------------------------------------------------------------------
TEST(ClassicContextTest, NextRoutineEmptyReturnsNull) {
  ClassicContext ctx("test_empty");
  EXPECT_EQ(ctx.NextRoutine(), nullptr);
}

// ----------------------------------------------------------------------
// 测试 2：NextRoutine 按优先级从高到低返回
// ----------------------------------------------------------------------
TEST(ClassicContextTest, NextRoutineRespectsPriority) {
  ClassicContext ctx("test_prio");

  // 入队 prio=5 和 prio=10
  auto cr_low = MakeReadyCRoutine(5, 1, "test_prio");
  auto cr_high = MakeReadyCRoutine(10, 2, "test_prio");
  ClassicContext::Enqueue(cr_low);
  ClassicContext::Enqueue(cr_high);

  // 第一个应返回高优先级
  auto first = ctx.NextRoutine();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->priority(), 10u);
  // 高优先级协程执行后标记 FINISHED，这样 NextRoutine 不会再返回它
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
TEST(ClassicContextTest, NextRoutineSkipsNonReady) {
  ClassicContext ctx("test_skip");

  // 入队一个 DATA_WAIT 协程和一个 READY 协程，优先级相同
  auto cr_wait = MakeReadyCRoutine(5, 1, "test_skip");
  cr_wait->SetState(RoutineState::DATA_WAIT);
  auto cr_ready = MakeReadyCRoutine(5, 2, "test_skip");

  ClassicContext::Enqueue(cr_wait);
  ClassicContext::Enqueue(cr_ready);

  // 应跳过 DATA_WAIT，返回 READY 的那个
  auto cr = ctx.NextRoutine();
  ASSERT_NE(cr, nullptr);
  EXPECT_EQ(cr->id(), 2u);
  cr->Release();
}

// ----------------------------------------------------------------------
// 测试 4：NextRoutine 返回的协程处于 Acquired 状态
// ----------------------------------------------------------------------
TEST(ClassicContextTest, NextRoutineAcquiresLock) {
  ClassicContext ctx("test_lock");

  auto cr = MakeReadyCRoutine(0, 1, "test_lock");
  ClassicContext::Enqueue(cr);

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
// 测试 5：Wait 阻塞直到 Notify 唤醒
// ----------------------------------------------------------------------
TEST(ClassicContextTest, WaitUnblocksOnNotify) {
  ClassicContext ctx("test_wait_notify");

  std::atomic<bool> wait_returned{false};
  std::thread waiter([&]() {
    ctx.Wait();
    wait_returned.store(true);
  });

  // 确保 waiter 进入 Wait
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(wait_returned.load());

  ClassicContext::Notify("test_wait_notify");
  waiter.join();
  EXPECT_TRUE(wait_returned.load());
}

// ----------------------------------------------------------------------
// 测试 6：Shutdown 唤醒阻塞的 Wait
// ----------------------------------------------------------------------
TEST(ClassicContextTest, ShutdownUnblocksWait) {
  ClassicContext ctx("test_shutdown");

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
// 测试 7：RemoveCRoutine 按 id 移除
// ----------------------------------------------------------------------
TEST(ClassicContextTest, RemoveCRoutineById) {
  ClassicContext ctx("test_remove");

  auto cr1 = MakeReadyCRoutine(5, 100, "test_remove");
  auto cr2 = MakeReadyCRoutine(5, 200, "test_remove");
  ClassicContext::Enqueue(cr1);
  ClassicContext::Enqueue(cr2);

  // 移除 cr1
  EXPECT_TRUE(ClassicContext::RemoveCRoutine(cr1));

  // 队列中应只剩 cr2
  auto out = ctx.NextRoutine();
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->id(), 200u);
  // 标记 FINISHED 后 Release，NextRoutine 不再返回它
  out->SetState(RoutineState::FINISHED);
  out->Release();

  // 再取时只检查当前组；已消费的 cr2 不应再次从其共享队列返回。
  bool cr2_gone = true;
  for (int i = 0; i < 100; ++i) {
    auto cr = ctx.NextRoutine();
    if (!cr) break;
    if (cr->id() == 200u) {
      cr2_gone = false;
      cr->Release();
      break;
    }
    cr->SetState(RoutineState::FINISHED);
    cr->Release();
  }
  EXPECT_TRUE(cr2_gone);
}

// ----------------------------------------------------------------------
// 测试 8：RemoveCRoutine 不存在的 id 返回 false
// ----------------------------------------------------------------------
TEST(ClassicContextTest, RemoveCRoutineNotFound) {
  ClassicContext ctx("test_remove_notfound");

  auto cr = MakeReadyCRoutine(0, 999);
  // 不入队直接 Remove 应返回 false
  EXPECT_FALSE(ClassicContext::RemoveCRoutine(cr));
}

TEST(ClassicContextTest, DoesNotConsumeAnotherGroupsQueue) {
  ClassicContext owner("test_owner_only");
  ClassicContext other("test_other_only");

  auto cr = MakeReadyCRoutine(5, 1, "test_owner_only");
  ClassicContext::Enqueue(cr);

  EXPECT_EQ(other.NextRoutine(), nullptr);
  auto out = owner.NextRoutine();
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->id(), 1u);
  out->Release();
}

TEST(ClassicContextTest, RemoveWaitsForAcquiredRoutine) {
  ClassicContext ctx("test_remove_acquired");
  auto cr = MakeReadyCRoutine(5, 1, "test_remove_acquired");
  ClassicContext::Enqueue(cr);

  auto acquired = ctx.NextRoutine();
  ASSERT_NE(acquired, nullptr);
  std::atomic<bool> removed{false};
  std::thread remover([&]() {
    removed.store(ClassicContext::RemoveCRoutine(cr));
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(removed.load());
  acquired->Release();
  remover.join();
  EXPECT_TRUE(removed.load());
  EXPECT_EQ(ctx.NextRoutine(), nullptr);
}

}  // namespace scheduler
}  // namespace minicyber
