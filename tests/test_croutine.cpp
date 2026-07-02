#include <gtest/gtest.h>
#include "minicyber/croutine/croutine.h"
#include <atomic>
#include <chrono>
#include <iostream>

namespace {
// 测试辅助：记录协程走过的状态轨迹
static std::vector<minicyber::RoutineState> g_trace;
}  // namespace

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
