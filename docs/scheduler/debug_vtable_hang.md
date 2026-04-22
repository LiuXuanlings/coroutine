# GDB 与 strace 调试实录：虚函数表导致的程序卡死

## 问题现象

程序在创建 `IOManager` 实例后卡死，无法退出。

```cpp
// 测试代码
sylar::IOManager iom;
iom.schedule(some_task);
// 程序卡在这里，无法继续
```

---

## 第一阶段：使用 strace 观察系统调用

### 运行 strace

```bash
strace -f ./test_iomanager 2>&1 | head -100
```

### 观察到的现象

```
[pid 12345] futex(0x7f..., FUTEX_WAIT_PRIVATE, 0, NULL <unfinished ...>
[pid 12346] epoll_wait(4,  <unfinished ...>
[pid 12347] epoll_wait(4,  <unfinished ...>
[pid 12348] epoll_wait(4,  <unfinished ...>
...
```

**关键发现：**
- 主线程阻塞在 `futex` 等待（`join` 等待工作线程退出）
- 工作线程阻塞在 `epoll_wait`（等待 IO 事件）

**问题定位：** 工作线程没有被正确唤醒，导致 `join` 永远阻塞。

---

## 第二阶段：使用 GDB 深入分析

### 启动 GDB

```bash
gdb ./test_iomanager
(gdb) run
# 等待程序卡住后按 Ctrl+C
```

### 查看所有线程状态

```gdb
(gdb) info threads
  Id   Target Id         Frame 
  1    Thread 0x7f... (main)  0x00007f... in pthread_join 
  2    Thread 0x7f...         0x00007f... in epoll_wait 
  3    Thread 0x7f...         0x00007f... in epoll_wait 
  ...
```

**发现：** 主线程在 `pthread_join`，工作线程在 `epoll_wait`。

### 检查工作线程调用栈

```gdb
(gdb) thread 2
(gdb) bt
#0  0x00007f... in epoll_wait () from /lib/x86_64-linux-gnu/libc.so.6
#1  0x000055... in sylar::Scheduler::run() at src/scheduler.cpp:72
#2  0x000055... in std::__invoke_impl<void, ...> ...
```

**关键发现：** 工作线程调用的是 `Scheduler::run()`，而不是 `IOManager::run()`！

### 验证虚函数调用

```gdb
(gdb) frame 1
(gdb) info locals
m_is_stopping = false
```

工作线程执行的是 `Scheduler::run()` 的逻辑（使用条件变量），而不是 `IOManager::run()` 的逻辑（使用 epoll）。

---

## 第三阶段：根因分析

### 问题根源：构造函数中的虚函数表

**C++ 对象构造顺序：**
```
1. 基类构造函数执行 → 虚函数表指向基类
2. 派生类成员初始化
3. 派生类构造函数体执行 → 虚函数表指向派生类
```

**原代码的问题：**

```cpp
// scheduler.cpp (原代码)
Scheduler::Scheduler() {
    for (size_t i = 0; i < THREAD_COUNT; ++i) {
        m_threads.push_back(std::make_shared<Thread>(
            std::bind(&Scheduler::run, this),  // ⚠️ 在基类构造函数中创建线程
            "scheduler_" + std::to_string(i)
        ));
    }
}

// iomanager.cpp
IOManager::IOManager() : Scheduler() {  // 先调用基类构造函数
    m_epfd = epoll_create(1024);
    // 此时线程已经在跑 Scheduler::run() 了！
    // 因为构造 IOManager 时，虚函数表还指向 Scheduler
}
```

**时序问题：**

```
时间线：
─────────────────────────────────────────────────────────────────►
     │                    │                              │
     ▼                    ▼                              ▼
Scheduler() 构造     线程启动执行              IOManager() 构造完成
虚函数表→Scheduler    run() 调用                虚函数表→IOManager
                     Scheduler::run()          (但线程已经在跑错误的代码)
                     (条件变量等待)             (而不是 IOManager::run 的 epoll)
```

### 为什么会卡死？

1. `IOManager` 构造时，线程已经启动
2. 但此时虚函数表指向 `Scheduler`
3. 线程执行 `Scheduler::run()`，阻塞在条件变量 `m_cv.wait()`
4. `IOManager::tickle()` 往管道写数据，不会唤醒条件变量
5. `stop()` 设置 `m_is_stopping` 并 `notify_all()`
6. 但线程检查队列为空后又进入 `idle()`，继续 `wait()`
7. `join()` 永远等待，程序卡死

---

## 第四阶段：修复方案

### 方案：延迟线程启动

**核心思想：** 在派生类构造完成后再启动线程，确保虚函数表正确。

```cpp
// scheduler.h
class Scheduler {
public:
    void startThreads();  // 新增：显式启动线程
    
protected:
    bool m_threads_started = false;  // 防止重复启动
};

// scheduler.cpp
Scheduler::Scheduler() {
    // 空构造函数，不创建线程！
}

void Scheduler::startThreads() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_threads_started) return;
    m_threads_started = true;
    
    for (size_t i = 0; i < THREAD_COUNT; ++i) {
        m_threads.push_back(std::make_shared<Thread>(
            std::bind(&Scheduler::run, this),
            "scheduler_" + std::to_string(i)
        ));
    }
}

void Scheduler::schedule(Fiber::ptr fiber) {
    // ...
    startThreads();  // 第一次调度时启动，确保派生类已构造
    // ...
}

// iomanager.cpp
IOManager::IOManager() : Scheduler() {
    m_epfd = epoll_create(1024);
    // ...
    
    startThreads();  // 在派生类构造完成后启动
}
```

### 修复后的时序

```
时间线：
─────────────────────────────────────────────────────────────────►
     │                              │                    │
     ▼                              ▼                    ▼
Scheduler() 构造              IOManager() 构造完成   startThreads()
虚函数表→Scheduler            虚函数表→IOManager     线程启动
(不创建线程)                  (设置 epoll)          执行 IOManager::run()
                                                    (正确的 epoll_wait)
```

---

## 总结：调试方法论

### strace 的作用

- **宏观视角：** 看到程序阻塞在哪个系统调用
- **快速定位：** `epoll_wait` vs `futex` vs `read` 等

### GDB 的作用

- **微观视角：** 看到具体是哪个函数调用
- **多线程分析：** `info threads` 查看所有线程状态
- **调用栈分析：** `bt` 追溯函数调用链
- **验证假设：** 检查变量值、确认虚函数调用

### 调试流程

```
1. strace 观察系统调用 → 定位阻塞点
2. GDB 查看线程状态 → 确认哪个线程有问题
3. GDB 查看调用栈 → 定位具体函数
4. 分析代码逻辑 → 找出根因
5. 修复并验证
```

### C++ 虚函数陷阱总结

> **黄金法则：永远不要在构造函数或析构函数中调用虚函数（或启动可能调用虚函数的线程）。**

原因：构造/析构期间，虚函数表指向当前类，而非最终派生类。
