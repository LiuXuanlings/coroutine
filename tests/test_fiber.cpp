#include<gtest/gtest.h>
#include"sylar/fiber.h"
#include<iostream>

void run_in_fiber(){
    std::cout<<"run_in_fiber begin"<< std::endl;
    sylar::Fiber::GetThis()->sylar::Fiber::yield();
    std::cout<<"run_in_fiber end"<< std::endl;
    sylar::Fiber::GetThis()->sylar::Fiber::yield();
}


TEST(FiberTest, BasicSwap){
    sylar::Fiber::GetThis();
    std::cout<<"main begin"<< std::endl;

    sylar::Fiber::ptr fiber(new sylar::Fiber(run_in_fiber));

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
        sylar::Fiber::GetThis()->yield();
    }
}

TEST(FiberTest, SwapSpeed){
    // 初始化主协程
    sylar::Fiber::GetThis();

    // 创建子协程
    sylar::Fiber::ptr fiber(new sylar::Fiber(fiber_speed_func));

    uint64_t start = gettime_ns_fiber();
    
    // 开始乒乓切换
    for(int i = 0; i < SWAP_N; i++){
        // 切换到子协程
        fiber->resume();
    }
    
    uint64_t end = gettime_ns_fiber();

    // 计算单次切换时间 (一次 resume + 一次 yield 算 2 次上下文切换)
    double per_switch_ns = (double)(end - start) / (SWAP_N * 2);

    // 注意：这里打印的是纳秒(ns)，因为协程切换通常在几十纳秒级别
    printf("fiber average switch time: %.2f ns\n", per_switch_ns);
    // 为了和线程直观对比，也可以打印微秒(us)
    printf("fiber average switch time: %.4f us\n", per_switch_ns / 1000.0);
}