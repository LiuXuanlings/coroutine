#include "sylar/scheduler.h"
#include <unistd.h>
#include <cstring>
#include <fcntl.h>

namespace sylar {
FDContext* Scheduler::getFdContext(int fd) {
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

Scheduler::Scheduler() {
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
    pipe_ctx->event_context.events = EPOLLIN;
    pipe_ctx->event_context.callback = nullptr; // 管道仅用于唤醒，无需回调

    // 将管道读端注册为可读事件
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = pipe_ctx;
    epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_pipe_read, &ev);

    for (size_t i = 0; i < THREAD_COUNT; ++i) {
        // &Scheduler::run:
        // run is a non-static member function, so its type is member-function pointer,
        // need address-of form "&Class::method".
        // std::bind(&Scheduler::run, this) binds current object pointer as implicit this,
        // converting member call into zero-arg callable matching Thread callback signature.
        // vector stores Thread::ptr, so use make_shared<Thread>(...) for shared ownership.
        m_threads.push_back(std::make_shared<Thread>(std::bind(&Scheduler::run, this), "scheduler_" + std::to_string(i)));
    }
}

void Scheduler::addEvent(int fd, int events, std::function<void()> callback) {
    // 从 vector 统一管理器中获取
    FDContext* fd_ctx = getFdContext(fd);
    fd_ctx->event_context.events = events;
    fd_ctx->event_context.callback = callback;

    // 创建 epoll_event，把 FDContext 保存在 epoll_event.data.ptr 指针里面
    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = fd_ctx;

    // 通过 epoll_ctl ADD 注册到 epoll 实例上
    epoll_ctl(m_epfd, EPOLL_CTL_ADD, fd, &ev);
}

void Scheduler::run() {
    // 重点注释：
    // 为当前工作线程初始化主协程，否则后续 fiber->resume() 无法切回该线程主协程。
    Fiber::GetThis();

    // 事件向量，用于收集就绪的 FD
    struct epoll_event events[1024];

    while (true) {
        Fiber::ptr fiber;

        {
            std::unique_lock<std::mutex> lock(m_mutex);

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
            while (m_queue.empty() && !m_is_stopping) {
                lock.unlock();

                // epoll_wait 等待就绪事件
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

                        // ==========================================
                        // 【防御死循环】：触发后立刻将该事件从 epoll 中删除！
                        // 这样即使数据没被读完，epoll 也不会再报了，直到用户重新 addEvent
                        // ==========================================
                        epoll_ctl(m_epfd, EPOLL_CTL_DEL, fd_ctx->fd, nullptr);
                        // 清理当前上下文的记录
                        fd_ctx->event_context.events = 0;
                        
                        //封装成一个 fiber，然后调用 schedule 函数
                        Fiber::ptr fiber_from_event(new Fiber(fd_ctx->event_context.callback));
                        schedule(fiber_from_event);
                    }
                }

                lock.lock();
            }

            if (m_is_stopping && m_queue.empty()) {
                // 接力唤醒机制
                // 临死前再往管道里写一个字符，叫醒下一个可能还在 epoll_wait 沉睡的兄弟
                char c = 'E'; // E 代表 Exit
                write(m_pipe_write, &c, 1);
                break;
            }

            fiber = m_queue.front();
            m_queue.pop();
        }

        // 重点注释：
        // 这里必须"先出队并释放锁，再执行 fiber->resume()"。
        // 原因是 resume() 会真正执行用户任务 如果持锁执行，会长时间占用队列锁，导致其他生产者/消费者线程无法访问队列。
        if (fiber) {
            fiber->resume();
        }
    }
}

void Scheduler::schedule(Fiber::ptr fiber) {
    if (!fiber) {
        return;
    }

    bool queue_was_empty = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 入队前检查是否为空：
        // 若原本为空，说明可能所有工作线程都在 wait，需要唤醒一个来消费新任务。
        queue_was_empty = m_queue.empty();
        m_queue.push(fiber);
    }

    if (queue_was_empty) {
        // 往管道写端写一个字符，唤醒其他线程
        char c = 'T';
        write(m_pipe_write, &c, 1);
    }
}

void Scheduler::stop() {
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
