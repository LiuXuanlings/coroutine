#include "sylar/scheduler.h"

namespace sylar {


Scheduler::Scheduler() {
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

void Scheduler::run() {
    // 重点注释：
    // 为当前工作线程初始化主协程，否则后续 fiber->resume() 无法切回该线程主协程。
    Fiber::GetThis();

    while (true) {
        Fiber::ptr fiber;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            // while而不是if的原因：
            // wait 可能被虚假唤醒（spurious wakeup），操作系统为了简化条件变量的底层实现并提高效率，允许wait在没有收到notify的情况下返回，所以必须使用while循环来重新检查条件。
            //while (m_queue.empty() && !m_is_stopping) {
            //    m_cv.wait(lock);
            //}
            //推荐写法：带谓词的wait，内部会自动循环检查条件，避免虚假唤醒问题。
            m_cv.wait(lock, [this] { return !m_queue.empty() || m_is_stopping; });

            if (m_is_stopping && m_queue.empty()) {
                break;
            }

            fiber = m_queue.front();
            m_queue.pop();
        }

        // 重点注释：
        // 这里必须“先出队并释放锁，再执行 fiber->resume()”。
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
        m_cv.notify_one();
    }
}

void Scheduler::stop() {
    {
        // m_is_stopping 的赋值同样需要保护，必须在锁内进行
        std::lock_guard<std::mutex> lock(m_mutex);
        m_is_stopping = true;
    }

    // 重点注释2：
    // stop 时必须 notify_all，而不是 notify_one。
    // 因为可能有多个工作线程都阻塞在 wait；若只唤醒一个，其它线程可能永远睡眠，
    // join 时主线程将一直等待。
    m_cv.notify_all();

    // 通过 join 可以“确认线程已退出 run()”：
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
}

} // namespace sylar
