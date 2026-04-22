#include "sylar/iomanager.h"
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace sylar {

FDContext* IOManager::getFdContext(int fd) {
    std::lock_guard<std::mutex> lock(m_fd_mutex);

    // 如果当前 vector 的大小装不下这个 fd，就扩容
    if (m_fd_contexts.size() <= (size_t)fd) {
        // 扩容到 fd 的 1.5 倍，用 nullptr 填充新空间
        m_fd_contexts.resize(fd * 1.5, nullptr);
    }

    // 如果这个 fd 对应的上下文还没有分配过，就 new 一个
    if (!m_fd_contexts[fd]) {
        m_fd_contexts[fd] = new FDContext();
        m_fd_contexts[fd]->fd = fd;
    }

    return m_fd_contexts[fd];
}

IOManager::IOManager() : Scheduler() {
    // 创建 epoll 实例
    m_epfd = epoll_create(1024);

    // 创建管道，用于唤醒其他线程
    int pipefd[2];
    pipe(pipefd);
    m_pipe_read = pipefd[0];
    m_pipe_write = pipefd[1];

    // 将管道读端设置为非阻塞模式
    // 必须设置！否则read会永久阻塞，导致调度线程无法退出
    int flags = fcntl(m_pipe_read, F_GETFL, 0);
    fcntl(m_pipe_read, F_SETFL, flags | O_NONBLOCK);

    // 封装管道事件上下文，存入epoll的data.ptr，避免指针失效
    FDContext* pipe_ctx = getFdContext(m_pipe_read);
    pipe_ctx->read_event_context.events = EPOLLIN;
    pipe_ctx->read_event_context.callback = nullptr; // 管道仅用于唤醒，无需回调

    // 将管道读端注册为可读事件
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = pipe_ctx;
    epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_pipe_read, &ev);

    // 在派生类构造完成后启动线程，确保虚函数表正确指向 IOManager
    startThreads();
}

IOManager::~IOManager() {
    stop();

    // 遍历 vector，把所有 new 出来的内存 delete 掉
    for (auto ctx : m_fd_contexts) {
        if (ctx) {
            delete ctx;
        }
    }
    m_fd_contexts.clear();
}

void IOManager::addEvent(int fd, int events, std::function<void()> callback) {
    // 1. 无效 FD 保护
    if (fd < 0) return;

    // 从 vector 统一管理器中获取
    FDContext* fd_ctx = getFdContext(fd);
    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // 如果该 Context 之前没事件，就是 ADD；否则就是 MOD (修改),
    // 因为同一个 FD 可能先注册了读事件，后又注册了写事件，或者反过来。
    // 或者用户先注册了读事件，后来又修改了回调函数，这些都属于修改而不是新增。
    // 如果全部用ADD，当同一个 FD 重复注册时，内核会报错 EEXIST；
    // 如果全部用MOD，当第一次注册时内核会报错 ENOENT。
    // 计算旧的事件掩码
    int old_events = fd_ctx->read_event_context.events | fd_ctx->write_event_context.events;
    int op = (old_events == 0) ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    // 计算新的事件掩码（基于当前状态和传入的events参数）
    int new_read_events = (events & EPOLLIN) ? EPOLLIN : fd_ctx->read_event_context.events;
    int new_write_events = (events & EPOLLOUT) ? EPOLLOUT : fd_ctx->write_event_context.events;
    int new_events = new_read_events | new_write_events;

    // 创建 epoll_event，把 FDContext 保存在 epoll_event.data.ptr 指针里面
    struct epoll_event ev;
    ev.events = new_events;
    ev.data.ptr = fd_ctx;

    // 检查内核注册结果
    if (epoll_ctl(m_epfd, op, fd, &ev) < 0) {
        // 如果失败了（比如 fd 已经关了），直接退出，不修改 fd_ctx 的状态
        perror("epoll_ctl failed");
        return;
    }

    // 只有内核注册成功，才更新内存中的状态
    if (events & EPOLLIN) {
        fd_ctx->read_event_context.events = EPOLLIN;
        fd_ctx->read_event_context.callback = callback;
    }
    if (events & EPOLLOUT) {
        fd_ctx->write_event_context.events = EPOLLOUT;
        fd_ctx->write_event_context.callback = callback;
    }
}

void IOManager::tickle() {
    // 往管道写端写一个字符，唤醒其他线程
    char c = 'T';
    write(m_pipe_write, &c, 1);
}

void IOManager::run() {
    // 重点注释：
    // 为当前工作线程初始化主协程，否则后续 fiber->resume() 无法切回该线程主协程。
    Fiber::GetThis();

    // 事件向量，用于收集就绪的 FD
    struct epoll_event events[1024];

    while (true) {
        Fiber::ptr fiber;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (!m_queue.empty()) {
                fiber = m_queue.front();
                m_queue.pop();
            }
        }

        if (fiber) {
            // 重点注释：
            // 这里必须"先出队并释放锁，再执行 fiber->resume()"。
            // 原因是 resume() 会真正执行用户任务，如果持锁执行，会长时间占用队列锁，
            // 导致其他生产者/消费者线程无法访问队列。
            fiber->resume();
            continue;
        }

        if (m_is_stopping) {
            // 接力唤醒机制
            // 临死前再往管道里写一个字符，叫醒下一个可能还在 epoll_wait 沉睡的兄弟
            char c = 'E'; // E 代表 Exit
            write(m_pipe_write, &c, 1);
            break;
        }

        // ------------------------------------------------------------------
        // 为什么必须将管道读端设置为非阻塞 (O_NONBLOCK)？
        // ------------------------------------------------------------------
        // 在基于 epoll 的多线程 Reactor 模式中，这是防止死锁的关键：
        //
        // 1. 配合 while 循环清空数据：
        //    在 run() 方法中，我们使用 while(read(...) > 0) 来彻底清空管道数据。
        //    如果管道是阻塞的，当最后一次把数据读完后，下一次 read 会因为没数据而
        //    永久挂起当前工作线程，导致线程彻底卡死，无法继续执行调度。
        //
        // 2. 应对多线程竞争：
        //    多个线程共享同一个 epoll_wait。主线程调用 stop()，循环写入了 N 个 'S' 到管道。
        //    或者某个线程连续 push 了任务，写了几个 'T'。此时，有多个线程同时被 epoll_wait 唤醒。
        //    线程A手快，先把这1个字节读走了。线程B慢了一步，当它去 read 时管道已经空了。
        //    如果管道是阻塞的，线程B就会被无辜卡死。非阻塞模式下，线程B只会收到 -1 (EAGAIN)
        //    并安全退出，继续回去工作。
        // ------------------------------------------------------------------
        // 如果队列为空，使用 epoll_wait 等待事件驱动唤醒
        int n = epoll_wait(m_epfd, events, 1024, -1);

        if (n > 0) {
            // 遍历就绪的 FD
            for (int i = 0; i < n; ++i) {
                FDContext* fd_ctx = static_cast<FDContext*>(events[i].data.ptr);

                // 如果 FD 是管道读端，读完管道内容并 continue
                if (fd_ctx->fd == m_pipe_read) {
                    char buf[256];
                    while (read(m_pipe_read, buf, sizeof(buf)) > 0) {
                        // 读完所有内容
                    }
                    continue;
                }

                std::lock_guard<std::mutex> lock(fd_ctx->mutex);
                int current_events = fd_ctx->read_event_context.events | fd_ctx->write_event_context.events;
                // 使用 vector 收集待调度的 fiber，因为一个 FD 可能同时触发读和写事件
                // 例如 socket 缓冲区既有数据可读又有空间可写时，epoll_wait 会同时返回 EPOLLIN | EPOLLOUT
                std::vector<Fiber::ptr> fibers_to_schedule;

                // 处理读事件
                if (events[i].events & EPOLLIN) {
                    if (fd_ctx->read_event_context.callback) {
                        fibers_to_schedule.push_back(std::make_shared<Fiber>(fd_ctx->read_event_context.callback));
                    }
                    fd_ctx->read_event_context.events = 0;
                }
                // 处理写事件
                if (events[i].events & EPOLLOUT) {
                    if (fd_ctx->write_event_context.callback) {
                        fibers_to_schedule.push_back(std::make_shared<Fiber>(fd_ctx->write_event_context.callback));
                    }
                    fd_ctx->write_event_context.events = 0;
                }
                // 处理错误和挂起事件（EPOLLERR 和 EPOLLHUP）
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    // 清除读写事件，避免后续重复触发
                    fd_ctx->read_event_context.events = 0;
                    fd_ctx->write_event_context.events = 0;
                    // 这里可以添加错误回调，但当前设计没有单独的错误回调
                }

                int new_events = fd_ctx->read_event_context.events | fd_ctx->write_event_context.events;
                if (new_events != current_events) {
                    if (new_events == 0) {
                        epoll_ctl(m_epfd, EPOLL_CTL_DEL, fd_ctx->fd, nullptr);
                    } else {
                        struct epoll_event ev;
                        ev.events = new_events;
                        ev.data.ptr = fd_ctx;
                        epoll_ctl(m_epfd, EPOLL_CTL_MOD, fd_ctx->fd, &ev);
                    }
                }

                // 调度所有产生的 fiber
                for (const auto& f : fibers_to_schedule) {
                    if (f) {
                        schedule(f);
                    }
                }
            }
        }
    }
}

void IOManager::stop() {
    {
        // m_is_stopping 的赋值同样需要保护，必须在锁内进行
        std::lock_guard<std::mutex> lock(m_mutex);
        m_is_stopping = true;
    }

    // 往管道写端写数据，唤醒可能阻塞在 epoll_wait 的线程
    char c = 'S';
    write(m_pipe_write, &c, 1);

    // 通过 join 可以"确认线程已退出 run()"：
    // join 返回的时刻，目标线程已经结束执行并退出。
    // 这就是 stop 如何判断所有线程都退出的依据。
    for (auto& thread : m_threads) {
        if (thread) {
            thread->join();
        }
    }

    for (auto& thread : m_threads) {
        thread.reset();
    }
    // vector::clear(): destroys all elements and sets size to 0, but capacity remains unchanged.
    m_threads.clear();

    // 关闭 epoll 和管道
    if (m_epfd >= 0) {
        close(m_epfd);
    }
    if (m_pipe_read >= 0) {
        close(m_pipe_read);
    }
    if (m_pipe_write >= 0) {
        close(m_pipe_write);
    }
}

} // namespace sylar
