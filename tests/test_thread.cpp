#include<gtest/gtest.h>
#include"sylar/thread.h"
#include<iostream>
#include<vector>

int fd1[2],fd2[2];

void thread_func(){
    std::cout<< "Thread name: "<<sylar::Thread::GetThis()->getName()<<std::endl;
}

void thread_func_2(){
    char c=0;
    while(1){
        read(fd1[0], &c, 1);//wait for the main thread
        write(fd2[1], &c, 1);//wake up the main thread
    }
}

//ns timing
static inline uint64_t gettime_ns(){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;//time value -> time stamp
}

#define N 1000000

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

TEST(ThreadTest, SwapSpeed){
    pipe(fd1);
    pipe(fd2);
    sylar::Thread::ptr thr(new sylar::Thread(thread_func_2,"other"));

    char c=0;
    uint64_t start = gettime_ns();
    for(int i=0;i<N;i++){
        write(fd1[1],&c,1);
        read(fd2[0],&c,1);
    }
    uint64_t end = gettime_ns();

    double per_switch = (double) (end-start)/(N*2);

    printf("average switch time: %.2f us\n",per_switch/1000);
}