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
