#ifndef SYLAR_SCHEDULER_H
#define SYLAR_SCHEDULER_H

#include "sylar/thread.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <vector>

namespace sylar {

class Scheduler {
public:
    static constexpr size_t THREAD_COUNT = 32;

    Scheduler();
    ~Scheduler() = default;

    static Scheduler* GetInstance();

    void run();
    void schedule(std::function<void()> task);

private:
    std::vector<Thread::ptr> m_threads;
    std::queue<std::function<void()>> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;

    static thread_local std::atomic<Scheduler*> t_scheduler;
    static std::mutex s_singleton_mutex;
};

} // namespace sylar

#endif