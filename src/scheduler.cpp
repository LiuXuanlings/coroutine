#include "sylar/scheduler.h"
#include <unistd.h>

namespace sylar {

Scheduler::Scheduler() {
    // 注意：不在基类构造函数中创建线程！
    // 原因：派生类构造期间虚函数表指向基类，工作线程会错误调用 Scheduler::run() 而非 IOManager::run()
    // 线程创建延迟到派生类构造函数完成后，由派生类调用 startThreads()
}

void Scheduler::startThreads() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_threads_started) {
        return;  // 已经启动，避免重复启动
    }
    m_threads_started = true;

    for (size_t i = 0; i < THREAD_COUNT; ++i) {
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

    // 延迟启动线程：第一次调度任务时启动
    // 这确保派生类构造完成后再启动线程，虚函数表正确指向派生类
    startThreads();

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
