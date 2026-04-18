#ifndef SYLAR_SCHEDULER_H
#define SYLAR_SCHEDULER_H

#include "sylar/fiber.h"
#include "sylar/thread.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <sys/epoll.h>
#include <utility>
#include <vector>

namespace sylar {

// EventContext: 被挂起的 fiber 的表示方法
struct EventContext {
    int events;                              // 事件类型，如 EPOLLIN
    std::function<void()> callback;          // 回调函数
};

// FDContext: 从文件描述符到被挂起的 fiber 的映射
struct FDContext {
    int fd;                                  // 文件描述符本身
    EventContext read_event_context;         // 读事件的 event_context
    EventContext write_event_context;        // 写事件的 event_context
    std::mutex mutex;                       // 保护 EventContext 的互斥锁
};

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

    ~Scheduler() {
    // 遍历 vector，把所有 new 出来的内存 delete 掉
    for (auto ctx : m_fd_contexts) {
        if (ctx) {
            delete ctx;
        }
    }
    m_fd_contexts.clear();
}

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

    // 注册需要被驱动的事件
    void addEvent(int fd, int events, std::function<void()> callback);

    /* ==============================================================================
     * 为什么将构造函数开放为 Public？
     * ==============================================================================
     * 1. 解决单元测试中的“单例污染 (Singleton Pollution)”问题：
     *    如果使用 GetInstance() 进行测试，调用 stop() 会彻底销毁内部的 epfd 和线程池。
     *    这会导致后续的测试用例拿到一个“已死亡”的调度器而全部崩溃或超时。
     *    开放 public 构造函数后，每个 TEST 都可以独立 new 一个干净的 Scheduler，
     *    测试完毕后 delete，确保各个测试用例之间状态 100% 隔离。
     * 
     * 2. 解决异步竞态条件 (Race Condition) 与 Flaky Tests：
     *    在测试异步网络 IO (EPOLLIN / EPOLLOUT) 时，主线程如果跑得太快直接调用 stop()，
     *    工作线程将直接退出，导致回调根本无法执行。
     *    配合 Public 实例，我们在测试文件中引入了轻量级的原子变量(std::atomic)轮询等待机制。
     *    强制主线程死等工作线程处理完事件后，才能调用 stop()，彻底消灭了时过时不过的 Flaky Tests。
     * 
     * 3. 架构演进准备：
     *    在高性能服务器中，通常会为不同的业务模块或不同的 NUMA 节点创建多个独立的调度器实例，
     *    打破严格单例限制是走向多实例高并发架构的第一步。
     * ============================================================================== */
    Scheduler();
    
private:
    // Private constructor: only GetInstance can create Scheduler objects, enforcing singleton pattern.
    // Scheduler();
    FDContext* getFdContext(int fd);

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

    // epoll 实例的文件描述符
    int m_epfd = -1;

    // 管道的读端和写端文件描述符
    int m_pipe_read = -1;
    int m_pipe_write = -1;


     /* ==============================================================================
     * 统一管理所有的 FDContext 指针 (以 FD 的整数值作为数组下标)
     * ==============================================================================
     * 采用 vector<FDContext*> 统一管理带来四大业界标准的优势：
     * 
     * 1. 彻底解决内存泄漏 (生命周期闭环)：
     *    按需分配，每个 FD 对应同一块内存。无论触发多少次事件，只 new 一次。
     *    最终在 Scheduler 析构函数中循环遍历 vector 进行统一 delete，绝不遗漏。
     * 
     * 2. 隐形的“内存对象池” (极致复用)：
     *    Linux OS 的特性是“优先复用刚关闭的、较小的 FD 编号”。
     *    当旧连接断开，新连接到来并复用了旧的 fd 编号时，这里会发现 vector[fd] 
     *    已经分配过内存了，于是直接覆盖复用！极大减少了频繁 new/delete 的性能开销和内存碎片。
     * 
     * 3. O(1) 的超高查找效率：
     *    直接用操作系统的 fd 整数作为数组下标（内存偏移寻址），没有任何计算开销。
     *    性能完胜基于红黑树的 std::map(O(logN)) 或 哈希的 std::unordered_map。
     * 
     * 4. 为什么存指针 (FDContext*) 而不能存对象 (FDContext)？【避坑高危区】：
     *    我们会把上下文对象的地址通过 ev.data.ptr = fd_ctx 交给底层 epoll。
     *    - 如果使用 vector<FDContext>：当容量不足发生 resize 扩容时，底层数组会整体拷贝
     *      并搬迁到新的内存地址。此时传给 epoll 的所有原地址瞬间变成野指针，程序当场崩溃！
     *    - 使用 vector<FDContext*>：扩容时仅仅搬迁“指针本身”。而指针指向的那块 new 出来
     *      的堆内存乖乖呆在原地，传给 epoll 的地址永久绝对有效！
     * ============================================================================== */
    // 统一管理所有的 FDContext 指针
    std::vector<FDContext*> m_fd_contexts;
    // 保护 m_fd_contexts 扩容时的线程安全
    std::mutex m_fd_mutex; 
};

} // namespace sylar

#endif