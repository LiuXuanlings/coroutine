#include "sylar/iomanager.h"
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include <cstring>
#include <iostream>

namespace {

// 创建非阻塞socket pair
bool create_socket_pair(int fds[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(fds[i], F_GETFL, 0);
        fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
    }
    return true;
}

// 创建非阻塞管道
bool create_pipe_pair(int fds[2]) {
    if (pipe(fds) < 0) {
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(fds[i], F_GETFL, 0);
        fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
    }
    return true;
}


} // namespace

int main() {
    std::atomic<int> callback_count{0};
    int fds[2];
    create_socket_pair(fds);

    sylar::IOManager io;
    /*
    为什么会陷入死循环？这就是经典的 Epoll 水平触发（Level-Triggered, LT）陷阱。
    场景复现：
    主线程往 fd=4 写了 "test"。此时 fd=3 里有了 4 个字节，变为可读状态。
    Thread 2 在 epoll_wait 醒来，发现 fd=3 可读。
    它把你绑定的 callback 封装成 Fiber 加入队列，并调用 schedule()。
    schedule() 往唤醒管道 fd=7 写了一个 'T'。
    Thread 2 执行 fiber->resume()。（关键点来了：如果你的 callback 里面没有调用 read(fds[0], ...) 把那 4 个字节读出来！）
    Thread 2 回到 epoll_wait。
    死循环爆发： 因为你使用的是 Epoll 的默认模式（LT模式）。LT 的规则是：只要 FD 里面的数据没被读干净，Epoll 就会立刻、无限次地触发事件！
    于是 epoll_wait 瞬间返回。同时带回两个事件：
    fd=3 依然有 "test" 没读，触发！-> 又创建一个 Fiber，又往管道写个 'T'。
    fd=6 收到上次写的 'T'，触发！-> 进入 while 循环读出 'T'。
    不断重复上述过程……你的内存会被无限创建的 Fiber 瞬间撑爆。
    */
    io.addEvent(fds[0], EPOLLIN, [&callback_count]() {
        ++callback_count;
    });

    const char* msg = "test";
    write(fds[1], msg, strlen(msg));

    io.stop();

    close(fds[0]);
    close(fds[1]);

    printf("Callback count: %d\n", callback_count.load());

}