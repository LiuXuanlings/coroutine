#include <gtest/gtest.h>
#include "minicyber/iomanager.h"
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

TEST(IOManagerTest, BasicTask) {
    std::atomic<int> count{0};
    minicyber::IOManager* io = new minicyber::IOManager();
    for (int i = 0; i < 10; ++i) io->schedule([&count]() { ++count; });
    io->stop();
    EXPECT_EQ(count.load(), 10);
    delete io;
}

TEST(IOManagerTest, IOReadWrite) {
    std::atomic<bool> read_ok{false}, write_ok{false};
    int fds[2];
    ASSERT_TRUE(create_socket_pair(fds));

    minicyber::IOManager* io = new minicyber::IOManager();
    io->addEvent(fds[0], EPOLLIN, [&read_ok]() { read_ok = true; });
    io->addEvent(fds[1], EPOLLOUT, [&write_ok]() { write_ok = true; });

    write(fds[1], "t", 1); // 触发读事件

    EXPECT_TRUE(wait_for([&]() { return read_ok.load() && write_ok.load(); }));

    io->stop();
    close(fds[0]); close(fds[1]);
    delete io;
}

TEST(IOManagerTest, NestedScheduling) {
    std::atomic<bool> nested_done{false};
    int fds[2];
    ASSERT_TRUE(create_socket_pair(fds));

    minicyber::IOManager* io = new minicyber::IOManager();
    io->addEvent(fds[0], EPOLLIN, [&]() {
        io->schedule([&nested_done]() { nested_done = true; });
    });

    write(fds[1], "t", 1);
    EXPECT_TRUE(wait_for([&]() { return nested_done.load(); }));

    io->stop();
    close(fds[0]); close(fds[1]);
    delete io;
}

// ============================================================================
// 第二部分：鲁棒性与边缘情况测试 (Robustness & Edge Cases)
// ============================================================================、

/**
 * @test 无效 FD 测试
 * 验证：传入 -1 或非法 FD 时，系统调用报错不应导致程序崩溃。
 */
TEST(IOManagerTest, InvalidFD) {
    minicyber::IOManager* io = new minicyber::IOManager();
    // epoll_ctl 内部会报错，但 addEvent 应当优雅处理或忽略
    io->addEvent(-1, EPOLLIN, []() {});
    io->stop();
    delete io;
    SUCCEED();
}

/**
 * @test 空回调测试
 * 验证：如果注册了事件但 callback 为空，当事件触发时，run() 应当有判空逻辑，避免执行 Fiber 时崩溃。
 */
TEST(IOManagerTest, EmptyCallback) {
    int fds[2];
    ASSERT_TRUE(create_socket_pair(fds));
    minicyber::IOManager* io = new minicyber::IOManager();

    // 故意不传回调（假设代码中对 nullptr 有判空）
    io->addEvent(fds[0], EPOLLIN, nullptr);
    write(fds[1], "t", 1);

    usleep(10000); // 留出触发时间，确保不崩溃
    io->stop();
    close(fds[0]); close(fds[1]);
    delete io;
    SUCCEED();
}

/**
 * @test 重复注册测试
 * 验证：同一个 FD 重复调用 addEvent。正常应由 ADD 自动转为 MOD 或处理错误。
 * 验证：程序不应因为 epoll 报错而中断。
 */
TEST(IOManagerTest, DuplicateFD) {
    int fds[2];
    ASSERT_TRUE(create_socket_pair(fds));
    minicyber::IOManager* io = new minicyber::IOManager();

    io->addEvent(fds[0], EPOLLIN, []() {});
    io->addEvent(fds[0], EPOLLIN, []() {}); // 重复添加

    io->stop();
    close(fds[0]); close(fds[1]);
    delete io;
    SUCCEED();
}

/**
 * @test 注册已关闭的 FD
 * 验证：如果 FD 在 addEvent 之前被 close 了，epoll_ctl 会报错 EBADF，
 * 测试此时 scheduler 的资源清理逻辑是否稳健。
 */
TEST(IOManagerTest, PreClosedFD) {
    int fds[2];
    ASSERT_TRUE(create_socket_pair(fds));
    close(fds[0]); // 提前关闭

    minicyber::IOManager* io = new minicyber::IOManager();
    io->addEvent(fds[0], EPOLLIN, []() {});

    io->stop();
    close(fds[1]);
    delete io;
    SUCCEED();
}
