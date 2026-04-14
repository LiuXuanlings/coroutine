# 从“纯阻塞队列调度”向“事件驱动调度”演进

### 当前架构全貌
1. **主线程**
   - 负责业务触发、统一调用 `Scheduler::schedule(Fiber)`
   - 把投递的 **Fiber任务推入全局共享阻塞队列**

2. **独立线程池（一组Worker线程）**
   - 每个Worker线程内部绑定一条**常驻主协程/主循环**，跑 `Run()` 函数
   - `Run()` 逻辑：阻塞等待条件变量 → 抢共享队列里的Fiber → `resume` 执行Fiber
   - 有新Fiber入队就 `notify`，唤醒Worker去调度执行

核心链路：
> 主线程 schedule → 共享队列入队 → notify唤醒线程池Worker线程 → Worker主循环Run取出并执行Fiber

---
## 现在这套架构最容易踩的坑
### **共享队列全局单锁，并发瓶颈明显**
所有Worker抢同一个队列锁，线程一多就内卷排队。
优化方向：
- 短期：加**队列分片**，多队列分散竞争
- 中期：每个Worker配私有local队列，全局队列兜底，做简易work-stealing

### Fiber内阻塞IO卡死整个Worker线程
Worker是绑定OS线程的，如果Fiber里有：
`sleep/recv/connect/同步锁阻塞`
→ 整个OS Worker线程被挂起，没法调度其他Fiber
解决方案：
- 网络IO要hook/事件驱动封装，阻塞时主动 `yield` 切走Fiber

---
## 两个问题
### Fiber 在执行过程中，我需要通过什么样的 API 来知道它已经阻塞了?
没法**主动感知**Fiber要阻塞，核心是：**把原生阻塞read/connect等API封装成协程专属异步接口**，Fiber调用这个封装API时，手动触发`yield`让出执行权，注册IO事件到epoll，就标记当前Fiber已挂起阻塞，不是系统被动感知。

### 如果要 hook 一个网络 IO 的 API，需要用到什么函数？
Linux 下动态库 Hook 网络IO接口（read/recv/send/connect等），核心依赖这两个函数：
1. **`dlsym(RTLD_NEXT, "函数名")`**：拿到系统原生的真实API地址，隐藏原生实现；
2. **`LD_PRELOAD` 动态预加载机制**：注入自定义动态库，劫持库函数调用。
帮你**精准对齐+修正流程**，完全贴合你说的思路，一步不差：

---
# 核心流程
1. 编写 `hook_io.c` 编译成动态库，靠 **`LD_PRELOAD` 劫持系统符号 `read`**；
2. 内部全局保存：用 `dlsym(RTLD_NEXT, "read")` 获取**原生真实 read 地址**；
3. 程序调用 `read()` 实际走进你的**自定义劫持 read 函数**；
4. 在自定义 read 里做三步关键操作：
   - 第一步：把目标 fd **设置为非阻塞模式**；
   - 第二步：先尝试调用一次**原生非阻塞 read**：
     - 有数据直接返回读取结果；
     - 没数据（触发 EAGAIN/EWOULDBLOCK）就进入协程逻辑；
   - 第三步：把**当前运行的 Fiber 绑定这个 fd 注册到 epoll 事件监听**，执行 `yield` 切出让出 Worker 线程；
5. epoll 监听 fd 可读就绪后，再 `schedule` 唤醒对应 Fiber，回来再重试原生 read；
6. 上层看起来还是调用阻塞 `read`，底层全是你的异步Fiber调度逻辑。

---
# 纠正你一句小偏差
> 「把原生 read 包装成一个 fiber 进行 schedule」

更标准正确写法：
**不是包装原生read成Fiber**，而是：
当前正在执行的业务Fiber遇到读阻塞条件 → 挂存自身+注册事件 → yield离场；
等事件就绪 → 调度器重新schedule这个**原有挂起Fiber**回来重试读。

---
# 极简小结
✅ LD_PRELOAD 劫持入口
✅ dlsym 拿原生系统read
✅ fd 改非阻塞 + 预读试探
✅ 无数据则Fiber挂起+epoll监听+yield
✅ 事件回调唤醒Fiber重新执行


---
## 改造思路
- 基类 Scheduler：`Run()` 靠 **cond 阻塞等任务**
- 派生 `IOScheduler`：重写 idle 逻辑，把「等cond」换成「epoll_wait」
---
### 标准配套架构
1. **原有线程池Worker**：死循环 `epoll_wait(ET)`；
2. 事件就绪后：某些线程被唤醒，拿到就绪fd；
3. 映射：`fd → 挂起的Fiber`；
4. 被唤醒的线程调用 `IOScheduler::schedule(对应Fiber)` 丢进任务队列
5. 结束schedule的被唤醒Worker和其余Worker抢到Fiber，resume继续执行刚才卡住的IO逻辑。

## 两个问题
### 事件就绪后，哪些线程会被唤醒?
如果有 N 个 Worker 线程正阻塞在同一个 epoll 实例的 epoll_wait() 调用上，内核会根据调度策略唤醒其中的一个或多个线程。对于同一个 epoll 实例，现代 Linux 内核保证了 epoll_wait 返回的就绪 fd 列表在线程间是互斥的，不会重复。
### 其他线程怎么被唤醒？
这里的“其他线程”通常指两种情况，需要分别处理：

#### 情况 A：如何唤醒“正在 epoll_wait 阻塞”的线程来处理新投递的任务？
（例如：主线程调用 schedule(Fiber) 投递了一个纯计算任务，没有 IO 事件，但 Worker 线程都在 epoll_wait 里睡着，怎么让它们醒来取任务？）

解决方案：使用“事件通知机制”管道唤醒（Eventfd / Pipe）

这是高性能网络库的标准套路，通常被称为 wakeup_fd 或 tickle 机制。

1. 创建管道：在 IOScheduler 初始化时，创建一个 eventfd 或 pipe。
2. 注册监听：将这个 wakeup_fd 的读端注册到 epoll 中，监听可读事件。
3. 唤醒流程：
   - 当主线程调用 schedule() 往任务队列里放入新任务时，不仅要加锁入队，还要往 wakeup_fd 的写端写入一个字节（或 8 字节整数）。
   - 这个写操作会立即导致 wakeup_fd 变为可读。
   - epoll 检测到 wakeup_fd 可读，立即唤醒阻塞的 Worker 线程。
   - Worker 线程醒来后，发现是 wakeup_fd 事件，读取并清空数据（防止重复触发），然后转而去检查任务队列，取出新任务执行。

#### 情况 B：如何唤醒“其他空闲”的线程来分担负载？
（例如：线程 A 被唤醒处理一个耗时任务，如何叫醒线程 B 来帮忙处理其他就绪的 IO？）

解决方案：依赖 epoll 的多线程竞争模型。 通常不需要显式唤醒。只要所有 Worker 线程都在跑 epoll_wait，当有多个事件就绪时，内核会自然地唤醒多个线程。


