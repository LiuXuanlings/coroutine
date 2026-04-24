#include <gtest/gtest.h>
#include "minicyber/scheduler.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace {

std::atomic<int> g_print_count{0};
std::atomic<int> g_sleep_count{0};

void print_task() {
    ++g_print_count;
    std::cout << "print_task running" << std::endl;
}

void sleep_print_task() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ++g_sleep_count;
    std::cout << "sleep_print_task done" << std::endl;
}

} // namespace

TEST(SchedulerTest, ScheduleAndStop) {
    g_print_count.store(0);
    g_sleep_count.store(0);

    minicyber::Scheduler scheduler;

    for (int i = 0; i < 100; ++i) {
        scheduler.schedule(print_task);
        scheduler.schedule(sleep_print_task);
    }

    scheduler.stop();

    EXPECT_EQ(g_print_count.load(), 100);
    EXPECT_EQ(g_sleep_count.load(), 100);
}
