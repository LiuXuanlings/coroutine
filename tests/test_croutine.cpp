#include <gtest/gtest.h>
#include "minicyber/croutine/croutine.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>

namespace {
// 测试辅助：记录协程走过的状态轨迹
static std::vector<minicyber::RoutineState> g_trace;

minicyber::croutine::RoutineContext g_context;
char* g_main_sp = nullptr;
int g_context_phase = 0;

void ContextEntry() {
  g_context_phase = 1;
  minicyber::croutine::SwapContext(&g_context.sp, &g_main_sp);
  g_context_phase = 2;
  minicyber::croutine::SwapContext(&g_context.sp, &g_main_sp);
}
}  // namespace

TEST(RoutineContextTest, SwitchesBothDirectionsWithAbiAlignedEntry) {
  g_context = {};
  g_main_sp = nullptr;
  g_context_phase = 0;
  minicyber::croutine::MakeContext(&g_context, ContextEntry);

#if defined(__x86_64__)
  // After restoring rdi plus six callee-saved registers, ret enters the
  // function with rsp at stack_end - sizeof(void*), as required by SysV ABI.
  const uintptr_t expected_sp =
      reinterpret_cast<uintptr_t>(g_context.stack) +
      minicyber::croutine::STACK_SIZE - sizeof(void*);
  EXPECT_EQ(expected_sp % 16, 8u);
#endif

  minicyber::croutine::SwapContext(&g_main_sp, &g_context.sp);
  EXPECT_EQ(g_context_phase, 1);

  minicyber::croutine::SwapContext(&g_main_sp, &g_context.sp);
  EXPECT_EQ(g_context_phase, 2);
}

// ----------------------------------------------------------------------
// 测试 1：基础状态流转 READY -> DATA_WAIT -> READY -> FINISHED
// 模拟"因数据未就绪而主动挂起"的数据驱动场景
// ----------------------------------------------------------------------
TEST(CRoutineTest, DataWaitStateTransition) {
  g_trace.clear();
  minicyber::CRoutine::GetThis();  // 初始化主协程

  auto cr = std::make_shared<minicyber::CRoutine>([]() {
    // 协程启动后处于 READY
    g_trace.push_back(minicyber::CRoutine::GetThis()->State());
    // 模拟数据未就绪，主动让出并进入 DATA_WAIT
    minicyber::CRoutine::Yield(minicyber::RoutineState::DATA_WAIT);
    // 被唤醒后继续执行
    g_trace.push_back(minicyber::CRoutine::GetThis()->State());
  });

  // 初始状态应为 READY
  EXPECT_EQ(cr->State(), minicyber::RoutineState::READY);

  // 第一次 Resume：协程运行，记录 READY 后 Yield(DATA_WAIT)
  cr->Resume();
  EXPECT_EQ(cr->State(), minicyber::RoutineState::DATA_WAIT);

  // 模拟数据到达，由 DataNotifier 将状态改回 READY
  cr->SetState(minicyber::RoutineState::READY);
  EXPECT_EQ(cr->State(), minicyber::RoutineState::READY);

  // 第二次 Resume：协程恢复，记录 READY 后正常结束
  cr->Resume();
  EXPECT_EQ(cr->State(), minicyber::RoutineState::FINISHED);

  // 验证协程内部观察到的状态轨迹
  ASSERT_EQ(g_trace.size(), 2u);
  EXPECT_EQ(g_trace[0], minicyber::RoutineState::READY);
  EXPECT_EQ(g_trace[1], minicyber::RoutineState::READY);
}

// ----------------------------------------------------------------------
// 测试 2：Yield() 无参版本保留当前状态
// ----------------------------------------------------------------------
TEST(CRoutineTest, YieldPreservesState) {
  minicyber::CRoutine::GetThis();

  minicyber::RoutineState observed_inside;
  auto cr = std::make_shared<minicyber::CRoutine>([&]() {
    observed_inside = minicyber::CRoutine::GetThis()->State();
    minicyber::CRoutine::Yield();  // 不带状态，应保留
  });

  cr->Resume();
  // Yield() 不改状态，应仍为 READY（Resume 时被设为 READY）
  EXPECT_EQ(cr->State(), minicyber::RoutineState::READY);
  EXPECT_EQ(observed_inside, minicyber::RoutineState::READY);

  // 必须再次 Resume 让协程跑完，否则 MainFunc 中的 cur 永远冻结，
  // 其栈上的 shared_ptr 不会析构，导致 CRoutine 对象泄漏。
  cr->Resume();
  EXPECT_EQ(cr->State(), minicyber::RoutineState::FINISHED);
}

// ----------------------------------------------------------------------
// 测试 3：SLEEP 与 IO_WAIT 状态可被设置并恢复
// ----------------------------------------------------------------------
TEST(CRoutineTest, SleepAndWaitStates) {
  minicyber::CRoutine::GetThis();

  auto cr = std::make_shared<minicyber::CRoutine>([]() {
    minicyber::CRoutine::Yield(minicyber::RoutineState::SLEEP);
    minicyber::CRoutine::Yield(minicyber::RoutineState::IO_WAIT);
  });

  cr->Resume();
  EXPECT_EQ(cr->State(), minicyber::RoutineState::SLEEP);

  cr->SetState(minicyber::RoutineState::READY);
  cr->Resume();
  EXPECT_EQ(cr->State(), minicyber::RoutineState::IO_WAIT);

  cr->SetState(minicyber::RoutineState::READY);
  cr->Resume();
  EXPECT_EQ(cr->State(), minicyber::RoutineState::FINISHED);
}

// ----------------------------------------------------------------------
// 测试 4：多次 Yield/Resume 循环不泄漏（smoke test）
// ----------------------------------------------------------------------
TEST(CRoutineTest, MultipleYieldResumeCycle) {
  minicyber::CRoutine::GetThis();

  int counter = 0;
  auto cr = std::make_shared<minicyber::CRoutine>([&]() {
    for (int i = 0; i < 100; ++i) {
      ++counter;
      minicyber::CRoutine::Yield(minicyber::RoutineState::DATA_WAIT);
    }
  });

  for (int i = 0; i < 100; ++i) {
    cr->SetState(minicyber::RoutineState::READY);
    cr->Resume();
    ASSERT_EQ(cr->State(), minicyber::RoutineState::DATA_WAIT);
  }
  cr->SetState(minicyber::RoutineState::READY);
  cr->Resume();
  EXPECT_EQ(cr->State(), minicyber::RoutineState::FINISHED);
  EXPECT_EQ(counter, 100);
}

// =====================================================================
// 协程执行所有权与状态更新测试
// =====================================================================

// 测试：Acquire/Release 互斥
TEST(CRoutineTest, AcquireReleaseMutex) {
  minicyber::CRoutine::GetThis();
  auto cr = std::make_shared<minicyber::CRoutine>([]() {});

  // 首次 Acquire 应成功
  EXPECT_TRUE(cr->Acquire());
  // 再次 Acquire 应失败（锁已被占用）
  EXPECT_FALSE(cr->Acquire());
  // Release 后再 Acquire 应成功
  cr->Release();
  EXPECT_TRUE(cr->Acquire());
  cr->Release();
}

// 测试：UpdateState SLEEP 超时后转为 READY
TEST(CRoutineTest, UpdateStateSleepTimeout) {
  minicyber::CRoutine::GetThis();
  auto cr = std::make_shared<minicyber::CRoutine>([]() {});

  // 设置 SLEEP 状态，唤醒时间为过去 -> 应立即转 READY
  cr->SetState(minicyber::RoutineState::SLEEP);
  cr->set_wake_time(std::chrono::steady_clock::now() -
                    std::chrono::milliseconds(1));
  EXPECT_EQ(cr->UpdateState(), minicyber::RoutineState::READY);
}

// 测试：UpdateState SLEEP 未超时保持 SLEEP
TEST(CRoutineTest, UpdateStateSleepNotTimeout) {
  minicyber::CRoutine::GetThis();
  auto cr = std::make_shared<minicyber::CRoutine>([]() {});

  cr->SetState(minicyber::RoutineState::SLEEP);
  cr->set_wake_time(std::chrono::steady_clock::now() +
                    std::chrono::seconds(10));
  EXPECT_EQ(cr->UpdateState(), minicyber::RoutineState::SLEEP);
}

// 测试：SetUpdateFlag 使 DATA_WAIT 转 READY
TEST(CRoutineTest, UpdateStateDataWaitWithFlag) {
  minicyber::CRoutine::GetThis();
  auto cr = std::make_shared<minicyber::CRoutine>([]() {});

  cr->SetState(minicyber::RoutineState::DATA_WAIT);
  // 通知数据就绪
  cr->SetUpdateFlag();
  EXPECT_EQ(cr->UpdateState(), minicyber::RoutineState::READY);
}

// 测试：DATA_WAIT 无通知保持 DATA_WAIT
TEST(CRoutineTest, UpdateStateDataWaitNoFlag) {
  minicyber::CRoutine::GetThis();
  auto cr = std::make_shared<minicyber::CRoutine>([]() {});

  cr->SetState(minicyber::RoutineState::DATA_WAIT);
  // 不调用 SetUpdateFlag，状态应保持
  EXPECT_EQ(cr->UpdateState(), minicyber::RoutineState::DATA_WAIT);
}

// 测试：SetUpdateFlag 使 IO_WAIT 转 READY
TEST(CRoutineTest, UpdateStateIoWaitWithFlag) {
  minicyber::CRoutine::GetThis();
  auto cr = std::make_shared<minicyber::CRoutine>([]() {});

  cr->SetState(minicyber::RoutineState::IO_WAIT);
  cr->SetUpdateFlag();
  EXPECT_EQ(cr->UpdateState(), minicyber::RoutineState::READY);
}

// 测试：Stop 设置 FINISHED 状态
TEST(CRoutineTest, StopSetsFinished) {
  minicyber::CRoutine::GetThis();
  auto cr = std::make_shared<minicyber::CRoutine>([]() {});

  EXPECT_EQ(cr->State(), minicyber::RoutineState::READY);
  cr->Stop();
  EXPECT_EQ(cr->State(), minicyber::RoutineState::READY);
  EXPECT_EQ(cr->Resume(), minicyber::RoutineState::FINISHED);
  EXPECT_EQ(cr->State(), minicyber::RoutineState::FINISHED);
}

TEST(CRoutineTest, ResumeRejectsNonReadyState) {
  minicyber::CRoutine::GetThis();
  int runs = 0;
  auto cr = std::make_shared<minicyber::CRoutine>([&]() { ++runs; });

  cr->SetState(minicyber::RoutineState::DATA_WAIT);
  EXPECT_EQ(cr->Resume(), minicyber::RoutineState::DATA_WAIT);
  EXPECT_EQ(runs, 0);

  cr->Wake();
  EXPECT_EQ(cr->Resume(), minicyber::RoutineState::FINISHED);
  EXPECT_EQ(runs, 1);
  EXPECT_EQ(cr->Resume(), minicyber::RoutineState::FINISHED);
}

// 测试：元数据 getter/setter
TEST(CRoutineTest, MetadataGettersSetters) {
  minicyber::CRoutine::GetThis();
  auto cr = std::make_shared<minicyber::CRoutine>([]() {});

  cr->set_id(42);
  cr->set_name("test_routine");
  cr->set_priority(15);
  cr->set_group_name("default_grp");

  EXPECT_EQ(cr->id(), 42u);
  EXPECT_EQ(cr->name(), "test_routine");
  EXPECT_EQ(cr->priority(), 15u);
  EXPECT_EQ(cr->group_name(), "default_grp");
}

// 测试：processor_id 默认值与 set/get
// 默认 -1 表示"未绑定"，set 后 get 返回 set 的值。
TEST(CRoutineTest, ProcessorIdDefaultAndSet) {
  minicyber::CRoutine::GetThis();
  auto cr = std::make_shared<minicyber::CRoutine>([]() {});

  EXPECT_EQ(cr->processor_id(), -1);

  // set 后 get 返回 set 的值
  cr->set_processor_id(0);
  EXPECT_EQ(cr->processor_id(), 0);

  cr->set_processor_id(7);
  EXPECT_EQ(cr->processor_id(), 7);
}
