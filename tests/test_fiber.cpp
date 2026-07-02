#include<gtest/gtest.h>
#include"minicyber/fiber.h"
#include"minicyber/croutine/croutine.h"
#include<iostream>

void run_in_fiber(){
    std::cout<<"run_in_fiber begin"<< std::endl;
    minicyber::Fiber::GetThis()->minicyber::Fiber::yield();
    std::cout<<"run_in_fiber end"<< std::endl;
    minicyber::Fiber::GetThis()->minicyber::Fiber::yield();
}


TEST(FiberTest, BasicSwap){
    minicyber::Fiber::GetThis();
    std::cout<<"main begin"<< std::endl;

    minicyber::Fiber::ptr fiber(new minicyber::Fiber(run_in_fiber));

    fiber->resume();
    std::cout<<"main after resume 1"<< std::endl;

    fiber->resume();
    std::cout<<"main after resume 2"<< std::endl;

    fiber->resume();
    std::cout<<"main end"<< std::endl;
}

// 获取纳秒时间戳的辅助函数
static inline uint64_t gettime_ns_fiber(){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
}

#define SWAP_N 1000000

// 测试用的子协程函数
void fiber_speed_func(){
    for(int i = 0; i < SWAP_N; i++){
        // 切换回主协程
        minicyber::Fiber::GetThis()->yield();
    }
}

TEST(FiberTest, SwapSpeed){
    // 初始化主协程
    minicyber::Fiber::GetThis();

    // 创建子协程
    minicyber::Fiber::ptr fiber(new minicyber::Fiber(fiber_speed_func));

    uint64_t start = gettime_ns_fiber();
    
    // 开始乒乓切换
    for(int i = 0; i < SWAP_N; i++){
        // 切换到子协程
        fiber->resume();
    }

    if(fiber->getState() != minicyber::Fiber::TERM){
        fiber->resume(); // 确保子协程能正常结束
    }
    
    uint64_t end = gettime_ns_fiber();

    // 计算单次切换时间 (一次 resume + 一次 yield 算 2 次上下文切换)
    double per_switch_ns = (double)(end - start) / (SWAP_N * 2);

    // 注意：这里打印的是纳秒(ns)，因为协程切换通常在几十纳秒级别
    printf("fiber average switch time: %.2f ns\n", per_switch_ns);
    // 为了和线程直观对比，也可以打印微秒(us)
    printf("fiber average switch time: %.4f us\n", per_switch_ns / 1000.0);
}

// =====================================================================
// Fiber <-> CRoutine 桥接测试
// 验证旧 Fiber 类通过新接口支持 RoutineState 语义
// =====================================================================

// 测试：Yield(RoutineState::DATA_WAIT) 让 Fiber 进入 DATA_WAIT 视图
TEST(FiberBridgeTest, YieldWithDataWait) {
    minicyber::Fiber::GetThis();  // 初始化主协程

    minicyber::RoutineState observed;
    auto fiber = std::make_shared<minicyber::Fiber>([&]() {
        observed = minicyber::Fiber::GetThis()->GetRoutineState();
        // 模拟数据未就绪，用新接口让出
        minicyber::Fiber::Yield(minicyber::RoutineState::DATA_WAIT);
    });

    // 初始：INIT -> READY
    EXPECT_EQ(fiber->GetRoutineState(), minicyber::RoutineState::READY);

    fiber->resume();
    // 协程内观察到的应为 READY（EXEC 映射为 READY）
    EXPECT_EQ(observed, minicyber::RoutineState::READY);
    // Yield(DATA_WAIT) 后应为 DATA_WAIT
    EXPECT_EQ(fiber->GetRoutineState(), minicyber::RoutineState::DATA_WAIT);
    // 旧 state 应同步为 HOLD
    EXPECT_EQ(fiber->getState(), minicyber::Fiber::HOLD);

    // 模拟数据到达，用新接口恢复
    fiber->SetRoutineState(minicyber::RoutineState::READY);
    EXPECT_EQ(fiber->GetRoutineState(), minicyber::RoutineState::READY);
    EXPECT_EQ(fiber->getState(), minicyber::Fiber::INIT);

    fiber->resume();
    // 协程结束 -> FINISHED
    EXPECT_EQ(fiber->GetRoutineState(), minicyber::RoutineState::FINISHED);
    EXPECT_EQ(fiber->getState(), minicyber::Fiber::TERM);
}

// 测试：旧 yield() 后 GetRoutineState() 默认映射为 SLEEP
TEST(FiberBridgeTest, LegacyYieldMapsToSleep) {
    minicyber::Fiber::GetThis();

    auto fiber = std::make_shared<minicyber::Fiber>([]() {
        minicyber::Fiber::GetThis()->yield();  // 旧接口
    });

    fiber->resume();
    // 旧 yield() 后 m_state = HOLD，routine_state_ 未被显式设置，
    // 默认映射应返回 SLEEP
    EXPECT_EQ(fiber->GetRoutineState(), minicyber::RoutineState::SLEEP);
    EXPECT_EQ(fiber->getState(), minicyber::Fiber::HOLD);

    // 跑完以避免泄漏
    fiber->resume();
    EXPECT_EQ(fiber->GetRoutineState(), minicyber::RoutineState::FINISHED);
}

// 测试：SetRoutineState 双向同步
TEST(FiberBridgeTest, SetRoutineStateSyncsLegacyState) {
    minicyber::Fiber::GetThis();

    auto fiber = std::make_shared<minicyber::Fiber>([]() {
        minicyber::Fiber::Yield(minicyber::RoutineState::IO_WAIT);
    });

    fiber->resume();
    EXPECT_EQ(fiber->GetRoutineState(), minicyber::RoutineState::IO_WAIT);
    EXPECT_EQ(fiber->getState(), minicyber::Fiber::HOLD);

    // 用 SetRoutineState 把 IO_WAIT 改回 READY
    fiber->SetRoutineState(minicyber::RoutineState::READY);
    EXPECT_EQ(fiber->getState(), minicyber::Fiber::INIT);

    fiber->resume();
    EXPECT_EQ(fiber->GetRoutineState(), minicyber::RoutineState::FINISHED);
}