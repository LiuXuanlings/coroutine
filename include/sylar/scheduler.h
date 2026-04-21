#ifndef SYLAR_SCHEDULER_H
#define SYLAR_SCHEDULER_H

#include "sylar/fiber.h"
#include "sylar/thread.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <vector>

namespace sylar {

/**
 * Scheduler: 纯粹的协程调度引擎
 *
 * 职责：
 * - 管理线程池
 * - 维护任务队列
 * - 调度 Fiber 执行
 *
 * 设计模式：模板方法模式
 * - run() 定义主循环骨架
 * - idle() 是扩展点，子类可重写实现不同的等待策略
 */
class Scheduler {
public:
    // static constexpr:
    // 1) static: class-level single constant, not per-object (no repeated storage per Scheduler instance)
    // 2) constexpr: compile-time constant
    // 3) hardcoded to 32 in current design stage
    static constexpr size_t THREAD_COUNT = 32;

    Scheduler();
    // 基类采用虚析构，是因为我们可能会通过基类指针删除子类对象，
    // 如果不虚析构，子类的析构函数就不会被调用，可能导致资源泄漏。
    virtual ~Scheduler();

    // 启动调度循环（在子类线程中运行）
    virtual void run();

    // 停止调度器
    virtual void stop();

    // 核心调度接口：直接调度一个 Fiber 实例
    void schedule(Fiber::ptr fiber);

    // 便捷调度接口：把函数和参数 bind 成 void()，再封装为 Fiber 入队
    template<class F, class... Args>
    void schedule(F&& f, Args&&... args) {
        std::function<void()> cb = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        Fiber::ptr fiber(new Fiber(cb));
        schedule(fiber);
    }

    // 唤醒一个可能处于空闲状态的线程
    virtual void tickle();

protected:
    /**
     * idle(): 空闲等待扩展点
     *
     * 当任务队列为空时，工作线程调用 idle() 等待新任务。
     * 基类实现：使用条件变量阻塞等待。
     * 子类可重写：如 IOManager 用 epoll_wait 实现事件驱动等待。
     *
     * @return true 表示被唤醒后应该继续检查队列；
     *         false 表示调度器正在停止，线程应退出
     */
    virtual bool idle();

    // 是否正在停止
    bool isStopping() const { return m_is_stopping; }

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