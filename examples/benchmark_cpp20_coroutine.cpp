// C++20 coroutine capability probe. It is intentionally excluded from latency CSV.

#include <coroutine>
#include <iostream>

namespace {

class ProbeTask {
 public:
  struct promise_type {
    ProbeTask get_return_object() {
      return ProbeTask{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() { std::terminate(); }
  };

  explicit ProbeTask(std::coroutine_handle<promise_type> handle) : handle_(handle) {}
  ProbeTask(const ProbeTask&) = delete;
  ProbeTask(ProbeTask&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  ~ProbeTask() {
    if (handle_) handle_.destroy();
  }

  void Resume() { handle_.resume(); }
  bool Done() const { return handle_.done(); }

 private:
  std::coroutine_handle<promise_type> handle_;
};

ProbeTask Probe() { co_await std::suspend_always{}; }

}  // namespace

int main() {
  auto task = Probe();
  task.Resume();
  task.Resume();
  if (!task.Done()) return 1;
  std::cout << "cpp20_coroutine_probe,compiled_and_resumed\n";
  return 0;
}
