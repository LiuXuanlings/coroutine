#include "minicyber/transport/shm/condition_notifier.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

namespace minicyber {
namespace transport {

ConditionNotifier::~ConditionNotifier() { Shutdown(); }

bool ConditionNotifier::Init() {
  if (shutdown_) return false;
  if (event_fd_ >= 0) return true;  // 已初始化

  // 创建 eventfd：初值 0，CLOEXEC 防止泄漏到子进程（除非 fork 继承）
  // 不使用 EFD_NONBLOCK：Listen 通过 epoll_wait 阻塞，读时已就绪
  // 不使用 EFD_SEMAPHORE：默认计数模式，一次 read 清空计数，适合"有数据到达"语义
  event_fd_ = ::eventfd(0, EFD_CLOEXEC);
  if (event_fd_ < 0) return false;

  epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) {
    ::close(event_fd_);
    event_fd_ = -1;
    return false;
  }

  struct epoll_event ev;
  std::memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN;
  ev.data.fd = event_fd_;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event_fd_, &ev) < 0) {
    ::close(event_fd_);
    ::close(epoll_fd_);
    event_fd_ = -1;
    epoll_fd_ = -1;
    return false;
  }
  return true;
}

bool ConditionNotifier::Notify() {
  if (shutdown_ || event_fd_ < 0) return false;
  uint64_t val = 1;
  ssize_t n = ::write(event_fd_, &val, sizeof(val));
  return n == static_cast<ssize_t>(sizeof(val));
}

bool ConditionNotifier::Listen(int timeout_ms) {
  if (shutdown_ || event_fd_ < 0 || epoll_fd_ < 0) return false;

  struct epoll_event events[1];
  int n = ::epoll_wait(epoll_fd_, events, 1, timeout_ms);
  if (n <= 0) {
    // n == 0 : 超时
    // n < 0 : 错误（EINTR 由调用者决定是否重试）
    return false;
  }
  // events[0].data.fd 应为 event_fd_，且 events & EPOLLIN
  if (events[0].data.fd == event_fd_ && (events[0].events & EPOLLIN)) {
    // 读出计数清零，避免后续重复唤醒
    uint64_t val = 0;
    ssize_t r = ::read(event_fd_, &val, sizeof(val));
    (void)r;
    return true;
  }
  return false;
}

void ConditionNotifier::Shutdown() {
  if (shutdown_) return;
  shutdown_ = true;
  if (event_fd_ >= 0) {
    ::close(event_fd_);
    event_fd_ = -1;
  }
  if (epoll_fd_ >= 0) {
    ::close(epoll_fd_);
    epoll_fd_ = -1;
  }
}

}  // namespace transport
}  // namespace minicyber