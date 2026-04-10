#ifndef SYLAR_SCHEDULER_H
#define SYLAR_SCHEDULER_H

#include "sylar/fiber.h"
#include "sylar/thread.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

namespace sylar {

class Scheduler {
public:
    // static constexpr:
    // 1) static: class-level single constant, not per-object (no repeated storage per Scheduler instance)
    // 2) constexpr: compile-time constant
    // 3) hardcoded to 32 in current design stage
    static constexpr size_t THREAD_COUNT = 32;

    ~Scheduler() = default;

    // static factory/accessor:
    // GetInstance must be callable before any Scheduler object exists,
    // so it cannot be a non-static member function.
    // Constructor only builds an object *after* instance creation has already been decided.
    static Scheduler* GetInstance();

    void run();
    void stop();

    // 核心调度接口：直接调度一个 Fiber 实例
    void schedule(Fiber::ptr fiber);

    // 便捷调度接口：把函数和参数 bind 成 void()，再封装为 Fiber 入队
    template<class F, class... Args>
    void schedule(F&& f, Args&&... args) {
        std::function<void()> cb = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        Fiber::ptr fiber(new Fiber(cb));
        schedule(fiber);
    }

private:
    // Private constructor: only GetInstance can create Scheduler objects, enforcing singleton pattern.
    Scheduler();

    // Thread pool container:
    // store Thread::ptr(shared_ptr) so thread objects have shared ownership/lifetime management.
    // In constructor, std::make_shared<Thread>(...) allocates control block + object efficiently.
    std::vector<Thread::ptr> m_threads;

    // FIFO task queue; each element is one Fiber task.
    std::queue<Fiber::ptr> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;

    // Stop flag: false by default, becomes true when stop() begins shutdown.
    bool m_is_stopping = false;

    // static members belong to class scope (shared by all Scheduler objects).
    // 不需要线程局部存储（thread_local）因为 Scheduler 是单例的，所有线程共享同一个实例。
    static std::atomic<Scheduler*> t_scheduler;

    // static global mutex used by GetInstance's double-checked locking critical section.
    static std::mutex s_singleton_mutex;
};

} // namespace sylar

#endif