#ifndef MINICYBER_IOMANAGER_H
#define MINICYBER_IOMANAGER_H

#include "minicyber/scheduler.h"
#include <functional>
#include <mutex>
#include <sys/epoll.h>
#include <vector>

namespace minicyber {

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
    std::mutex mutex;                        // 保护 EventContext 的互斥锁
};

/**
 * IOManager: 事件驱动的协程调度器
 *
 * 继承 Scheduler，在纯调度能力基础上增加：
 * - epoll IO 多路复用
 * - addEvent() 注册 IO 事件
 *
 * 工作原理：
 * - 重写 run()，用 epoll_wait 替换条件变量等待
 * - 当 IO 事件就绪时，将回调封装为 Fiber 调度执行
 */

/* ==============================================================================
 * 为什么将构造函数开放为 Public？
 * ==============================================================================
 * 1. 解决单元测试中的"单例污染 (Singleton Pollution)"问题：
 *    如果使用 GetInstance() 进行测试，调用 stop() 会彻底销毁内部的 epfd 和线程池。
 *    这会导致后续的测试用例拿到一个"已死亡"的调度器而全部崩溃或超时。
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
class IOManager : public Scheduler {
public:
    IOManager();
    ~IOManager() override;

    // 注册需要被驱动的事件
    void addEvent(int fd, int events, std::function<void()> callback);

    // 重写 run() 以加入 epoll 事件循环
    void run() override;

    // 重写 stop() 以清理 epoll 和管道资源
    void stop() override;

    // 重写 tickle() 以唤醒 epoll_wait
    void tickle() override;

private:
    FDContext* getFdContext(int fd);

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
     * 2. 隐形的"内存对象池" (极致复用)：
     *    Linux OS 的特性是"优先复用刚关闭的、较小的 FD 编号"。
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
     *    - 使用 vector<FDContext*>：扩容时仅仅搬迁"指针本身"。而指针指向的那块 new 出来
     *      的堆内存乖乖呆在原地，传给 epoll 的地址永久绝对有效！
     * ============================================================================== */
    std::vector<FDContext*> m_fd_contexts;
    // 保护 m_fd_contexts 扩容时的线程安全
    std::mutex m_fd_mutex;
};

} // namespace minicyber

#endif
