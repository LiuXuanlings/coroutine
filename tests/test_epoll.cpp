#include <gtest/gtest.h>
#include "sylar/scheduler.h"
#include <atomic>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>

namespace {

// 辅助：创建非阻塞 socket 对
bool create_socket_pair(int fds[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) return false;
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(fds[i], F_GETFL, 0);
        fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
    }
    return true;
}

// 辅助：轻量级轮询等待，用于同步，避免复杂同步原语撑破 ASan 协程栈
template <typename Condition>
bool wait_for(Condition cond, int timeout_ms = 500) {
    int elapsed = 0;
    while (!cond() && elapsed < timeout_ms) {
        usleep(1000); 
        elapsed++;
    }
    return cond();
}

} // namespace

// ============================================================================
// 第一部分：核心逻辑测试 (Core Logic)
// ============================================================================

TEST(SchedulerTest, BasicTask) {
    std::atomic<int> count{0};
    sylar::Scheduler* sc = new sylar::Scheduler();
    for (int i = 0; i < 10; ++i) sc->schedule([&count]() { ++count; });
    sc->stop();
    EXPECT_EQ(count.load(), 10);
    delete sc;
}

TEST(SchedulerTest, IOReadWrite) {
    std::atomic<bool> read_ok{false}, write_ok{false};
    int fds[2];
    ASSERT_TRUE(create_socket_pair(fds));

    sylar::Scheduler* sc = new sylar::Scheduler();
    sc->addEvent(fds[0], EPOLLIN, [&read_ok]() { read_ok = true; });
    sc->addEvent(fds[1], EPOLLOUT, [&write_ok]() { write_ok = true; });

    write(fds[1], "t", 1); // 触发读事件

    EXPECT_TRUE(wait_for([&]() { return read_ok.load() && write_ok.load(); }));
    
    sc->stop();
    close(fds[0]); close(fds[1]);
    delete sc;
}

TEST(SchedulerTest, NestedScheduling) {
    std::atomic<bool> nested_done{false};
    int fds[2];
    ASSERT_TRUE(create_socket_pair(fds));

    sylar::Scheduler* sc = new sylar::Scheduler();
    sc->addEvent(fds[0], EPOLLIN, [&]() {
        sc->schedule([&nested_done]() { nested_done = true; });
    });

    write(fds[1], "t", 1);
    EXPECT_TRUE(wait_for([&]() { return nested_done.load(); }));

    sc->stop();
    close(fds[0]); close(fds[1]);
    delete sc;
}
