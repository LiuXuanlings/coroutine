#include<gtest/gtest.h>
#include"sylar/thread.h"
#include<iostream>
#include<vector>

void thread_func(){
    std::cout<< "Thread name: "<<sylar::Thread::GetThis()->getName()<<std::endl;
}

TEST(ThreadTest, BasicCreation){
    std::vector<sylar::Thread::ptr> threads;
    for(int i=0;i<5;i++){
        sylar::Thread::ptr thr(new sylar::Thread(thread_func,"worker_"+std::to_string(i)));
        threads.push_back(thr);
    }

    for(auto &thr:threads){
        thr->join();
    }
}