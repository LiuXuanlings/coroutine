#ifndef MINICYBER_TRANSPORT_SHM_CONDITION_NOTIFIER_H_
#define MINICYBER_TRANSPORT_SHM_CONDITION_NOTIFIER_H_

#include <cstdint>

namespace minicyber {
namespace transport {

// =============================================================================
// ConditionNotifier：跨进程事件通知器（eventfd + epoll 简化版）
//
// 替代 CyberRT 的 System V SHM + 轮询方案，改用 Linux eventfd：
//   - eventfd 是 Linux 2.6.22+ 提供的轻量 IPC，一次 write 让对端 read 醒来
//   - 默认计数模式：write 的值累加，read 一次读出全部并清零
//   - 可被 fork 继承（子进程拿到同一个底层对象），适合跨进程唤醒
//
// 关键设计（与 IOManager 的融合点）：
//   Fd() 暴露 eventfd 文件描述符，Step 21 的 ShmReceiver 会把它注册进
//   底层 IOManager 的 epoll 树。当对端进程 Notify 时，本进程的
//   epoll_wait 醒来，直接唤醒关联协程，把 SHM 数据注入 DataDispatcher。
//
// 接口：
//   - Init()      : 创建 eventfd 与 epoll，并把 eventfd 加入 epoll
//   - Notify()    : 向 eventfd 写 1，唤醒所有监听者
//   - Listen(ms)  : epoll_wait 阻塞等待，timeout_ms=-1 表示永久等待
//                   返回 true 表示收到通知，false 表示超时或已 shutdown
//   - Fd()        : 返回 eventfd，供 IOManager 注册进 epoll
//   - Shutdown()  : 关闭两个 fd，幂等
// =============================================================================

class ConditionNotifier {
 public:
  ConditionNotifier() = default;
  ~ConditionNotifier();

  ConditionNotifier(const ConditionNotifier&) = delete;
  ConditionNotifier& operator=(const ConditionNotifier&) = delete;

  // 初始化：创建 eventfd 与 epoll，并把 eventfd 注册进 epoll
  bool Init();

  // 发出一次通知（向 eventfd 写 1）
  bool Notify();

  // 阻塞等待通知：
  //   timeout_ms = -1 : 永久等待
  //   timeout_ms = 0  : 非阻塞轮询
  //   timeout_ms > 0  : 等待至多 timeout_ms 毫秒
  // 返回 true 表示收到通知，false 表示超时或已 shutdown
  bool Listen(int timeout_ms = -1);

  // 供 IOManager 注册进 epoll 的关键接口
  int Fd() const { return event_fd_; }
  int EpollFd() const { return epoll_fd_; }

  // 关闭资源，幂等
  void Shutdown();

  bool IsShutdown() const { return shutdown_; }

 private:
  int event_fd_ = -1;
  int epoll_fd_ = -1;
  bool shutdown_ = false;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_SHM_CONDITION_NOTIFIER_H_