#ifndef SYLAR_THREAD_H
#define SYLAR_THREAD_H

#include <functional>
#include <memory>
#include <pthread.h>
#include <string>
#include <mutex>
#include <condition_variable>

namespace sylar {

class Semaphore {
public:
    explicit Semaphore(int count = 0);//
    void wait();
    void signal();

private:
    int m_count;
    std::mutex m_mutex;
    std::condition_variable m_cond;
};

class Thread {
public:
    using ptr = std::shared_ptr<Thread>;

    Thread(std::function<void()> cb, const std::string& name);
    ~Thread();

    pid_t getId() const { return m_id; }
    const std::string& getName() const { return m_name; }

    void join();

    static Thread* GetThis();
    static const std::string& GetName();
    static void SetName(const std::string& name);

private:
    static void* run(void* arg);

private:
    pid_t m_id = -1;//进程id
    pthread_t m_thread = 0;//线程句柄
    std::function<void()> m_cb;
    std::string m_name;
    Semaphore m_semaphore;
};

} // namespace sylar

#endif