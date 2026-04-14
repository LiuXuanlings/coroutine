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

    //利用局部静态变量实现单例模式，保证线程安全且只创建一个实例
    //在 C++ 中，类的静态成员函数（如 GetInstance）可以访问私有构造函数。但是必须把 GetInstance 的实现放在类定义内部（内联），或者确保编译器在处理 GetInstance 时能看到构造函数的定义。
    static Scheduler* GetInstance() {
        // C++11 保证 static 局部变量只会被初始化一次，且线程安全
        static Scheduler* s_instance = new Scheduler();
        return s_instance;
    }
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
};

} // namespace sylar

#endif