#include "sylar/scheduler.h"
#include <unistd.h>

namespace sylar {

Scheduler::Scheduler() {
    for (size_t i = 0; i < THREAD_COUNT; ++i) {
        // &Scheduler::run:
        // run is a non-static member function, so its type is member-function pointer,
        // need address-of form "&Class::method".
        // std::bind(&Scheduler::run, this) binds current object pointer as implicit this,
        // converting member call into zero-arg callable matching Thread callback signature.
        // vector stores Thread::ptr, so use make_shared<Thread>(...) for shared ownership.
        m_threads.push_back(std::make_shared<Thread>(
            std::bind(&Scheduler::run, this),
            "scheduler_" + std::to_string(i)
        ));
    }
}

Scheduler::~Scheduler() {
    stop();
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
        tickle();
    }
}

void Scheduler::tickle() {
    m_cv.notify_one();
}

bool Scheduler::idle() {
    // 基类实现：使用条件变量阻塞等待
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this] {
        return !m_queue.empty() || m_is_stopping;
    });
    // 如果是因停止而被唤醒，返回 false 告知调用者应退出
    return !m_is_stopping;
}

void Scheduler::run() {
    // 重点注释：
    // 为当前工作线程初始化主协程，否则后续 fiber->resume() 无法切回该线程主协程。
    Fiber::GetThis();

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

        // 队列为空，检查是否停止
        if (m_is_stopping) {
            break;
        }

        // 进入空闲等待
        if (!idle()) {
            break;
        }
    }
}

void Scheduler::stop() {
    {
        // m_is_stopping 的赋值同样需要保护，必须在锁内进行
        std::lock_guard<std::mutex> lock(m_mutex);
        m_is_stopping = true;
    }

    // 唤醒所有可能阻塞的线程
    m_cv.notify_all();

    // 通过 join 可以"确认线程已退出 run()"：
    // join 返回的时刻，目标线程已经结束执行并退出。
    // 这就是 stop 如何判断所有线程都退出的依据。
    for (auto& thread : m_threads) {
        if (thread) {
            thread->join();
        }
    }

    // vector::clear(): destroys all elements and sets size to 0, but capacity remains unchanged.
    m_threads.clear();
}

} // namespace sylar
