#include "sylar/thread.h"
#include <unistd.h>
#include <sys/syscall.h>

namespace sylar {

// -- 线程局部变量 --
static thread_local Thread* t_thread = nullptr;
static thread_local std::string t_thread_name = "UNKNOWN";

// -- Semaphore 实现 --
Semaphore::Semaphore(int count) : m_count(count) {}

void Semaphore::wait() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cond.wait(lock, [this]() { return m_count > 0; });
    --m_count;
}

void Semaphore::signal() {
    std::unique_lock<std::mutex> lock(m_mutex);
    ++m_count;
    m_cond.notify_one();
}

// -- Thread 实现 --
Thread* Thread::GetThis() { return t_thread; }
const std::string& Thread::GetName() { return t_thread_name; }
void Thread::SetName(const std::string& name) {
    if (t_thread) {
        t_thread->m_name = name;
    }
    t_thread_name = name;
}

Thread::Thread(std::function<void()> cb, const std::string& name)
    : m_cb(cb), m_name(name) {
    int rt = pthread_create(&m_thread, nullptr, run, this);//
    if (rt) {
        // 实际项目中应抛出异常
        throw std::logic_error("pthread_create error");
    }
    m_semaphore.wait(); // 等待新线程初始化完成
}

Thread::~Thread() {
    if (m_thread) {
        pthread_detach(m_thread);//将线程设为分离状态，线程终止后由系统自动回收资源。
    }
}

void Thread::join() {
    if (m_thread) {
        int rt = pthread_join(m_thread, nullptr);//以阻塞方式等待目标线程终止，并由当前线程主动回收其内核资源。
        if (rt) {
            throw std::logic_error("pthread_join error");
        }
        m_thread = 0;
    }
}

void* Thread::run(void* arg) {//适配pthread_create接口的中转函数，真实回调存在类成员里。
    Thread* thread = static_cast<Thread*>(arg);
    t_thread = thread;
    t_thread_name = thread->m_name;
    thread->m_id = syscall(SYS_gettid);
    pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());//ps -L -p pid

    std::function<void()> cb;
    cb.swap(thread->m_cb);//延长回调函数的的生命周期
    
    thread->m_semaphore.signal(); // 通知构造函数
    cb(); // 执行真正的回调
    return 0;
}

} // namespace sylar