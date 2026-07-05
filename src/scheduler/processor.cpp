#include "minicyber/scheduler/processor.h"

#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <chrono>

#include "minicyber/base/macros.h"
#include "minicyber/croutine/croutine.h"

namespace minicyber {
namespace scheduler {

Processor::Processor() {
  running_.store(true);
}

Processor::~Processor() {
  Stop();
}

// ----------------------------------------------------------------------
// Run: Processor 的调度主循环
// ----------------------------------------------------------------------
// 1. 记录本线程的 Linux TID（供 SetSchedPolicy 使用）
// 2. while running:
//    a. context_ 已绑定 -> NextRoutine()
//       - 有任务: 记录开始时间，Resume 协程
//       - 无任务: 清零时间戳，Wait() 阻塞等待新任务
//    b. context_ 未绑定 -> 在 cv_ctx_ 上等 10ms（避免空转）
//
// 注意：croutine->Release() 在 CyberRT 中用于释放 work-stealing 锁，
// 我们的 CRoutine 尚未引入该锁，暂跳过（Step 10 实现 Work-Stealing 时补上）。
// ----------------------------------------------------------------------
void Processor::Run() {
  tid_.store(static_cast<int>(syscall(SYS_gettid)));
  snap_shot_->processor_id.store(tid_.load());

  // 关键：为当前工作线程初始化主协程（t_thread_croutine）。
  // 否则 croutine->Resume() 会解引用 nullptr 的 t_thread_croutine 而崩溃。
  // 这与 Scheduler::run() 中调用 Fiber::GetThis() 的作用完全一致。
  CRoutine::GetThis();

  while (cyber_likely(running_.load())) {
    if (cyber_likely(context_ != nullptr)) {
      auto croutine = context_->NextRoutine();
      if (croutine) {
        snap_shot_->execute_start_time.store(
            std::chrono::steady_clock::now().time_since_epoch().count());
        croutine->Resume();
        // 释放工作窃取锁：NextRoutine() 中 Acquire 获取，Resume() 返回后
        // （协程 Yield 或执行完毕）必须 Release，否则下次 NextRoutine 无法
        // 再次 Acquire 该协程，导致 DATA_WAIT 协程被永久卡住。
        croutine->Release();
      } else {
        snap_shot_->execute_start_time.store(0);
        context_->Wait();
      }
    } else {
      // context_ 尚未绑定，短暂等待避免空转
      std::unique_lock<std::mutex> lk(mtx_ctx_);
      cv_ctx_.wait_for(lk, std::chrono::milliseconds(10));
    }
  }
}

// ----------------------------------------------------------------------
// Stop: 优雅停止
// ----------------------------------------------------------------------
// 1. running_ exchange(false)：若已是 false 说明已 Stop 过，直接返回
// 2. 通知 context Shutdown（唤醒可能阻塞在 Wait() 的线程）
// 3. 唤醒可能阻塞在 cv_ctx_ 的线程（context 未绑定场景）
// 4. join 线程
// ----------------------------------------------------------------------
void Processor::Stop() {
  if (!running_.exchange(false)) {
    return;
  }

  if (context_) {
    context_->Shutdown();
  }

  cv_ctx_.notify_one();
  if (thread_.joinable()) {
    thread_.join();
  }
}

// ----------------------------------------------------------------------
// BindContext: 绑定上下文并延迟启动线程
// ----------------------------------------------------------------------
// std::call_once 保证即使多次 BindContext 也只创建一个线程。
// 这与 CyberRT 的设计一致：线程生命周期与 Processor 绑定，
// context 可在运行中替换（虽然当前场景不这么做）。
// ----------------------------------------------------------------------
void Processor::BindContext(const std::shared_ptr<ProcessorContext>& context) {
  context_ = context;
  // std::once_flag类型标记thread_flag_，配合std::call_once保证内部lambda仅执行一次
  // 即使多线程并发调用BindContext，也只会创建一条Run工作线程，避免重复新建线程
  std::call_once(thread_flag_,
                 [this]() { thread_ = std::thread(&Processor::Run, this); });
}

// ----------------------------------------------------------------------
// Tid: 自旋等待直到线程 TID 就绪
// ----------------------------------------------------------------------
// Run() 在线程启动后立即设置 tid_，但调用方可能在 BindContext 后
// 立即需要 TID（如 SetSchedPolicy），此时 tid_ 可能还是 -1。
// 自旋等待是 CyberRT 的做法，延迟极短。
// ----------------------------------------------------------------------
std::atomic<pid_t>& Processor::Tid() {
  while (tid_.load() == -1) {
    std::this_thread::yield();
  }
  return tid_;
}

}  // namespace scheduler
}  // namespace minicyber
