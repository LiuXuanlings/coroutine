#include "sylar/scheduler.h"

namespace sylar {

thread_local std::atomic<Scheduler*> Scheduler::t_scheduler{nullptr};
std::mutex Scheduler::s_singleton_mutex;

Scheduler* Scheduler::GetInstance() {
    Scheduler* scheduler = t_scheduler.load(std::memory_order_acquire);
    if (scheduler == nullptr) {
        std::lock_guard<std::mutex> lock(s_singleton_mutex);
        scheduler = t_scheduler.load(std::memory_order_acquire);
        if (scheduler == nullptr) {
            scheduler = new Scheduler();
            t_scheduler.store(scheduler, std::memory_order_release);
        }
    }
    return scheduler;
}

Scheduler::Scheduler() {
    for (size_t i = 0; i < THREAD_COUNT; ++i) {
        m_threads.push_back(std::make_shared<Thread>(std::bind(&Scheduler::run, this), "scheduler_" + std::to_string(i)));
    }
}

void Scheduler::run() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_queue.empty()) {
                m_cv.wait(lock);
            }
            if (!m_queue.empty()) {
                task = m_queue.front();
                m_queue.pop();
            }
        }

        if (task) {
            schedule(task);
        }
    }
}

void Scheduler::schedule(std::function<void()> task) {
    if (task) {
        task();
    }
}

} // namespace sylar
