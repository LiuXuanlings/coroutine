#ifndef MINICYBER_SCHEDULER_PROCESSOR_H_
#define MINICYBER_SCHEDULER_PROCESSOR_H_

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "minicyber/scheduler/processor_context.h"

namespace minicyber {
namespace scheduler {

class Scheduler;  // 前置声明，避免与 processor.h 的循环依赖

// ----------------------------------------------------------------------
// Snapshot: Processor 运行时快照（供监控/调试用）
// ----------------------------------------------------------------------
struct Snapshot {
  std::atomic<uint64_t> execute_start_time{0};  // 当前协程开始执行的时间戳(ns)
  std::atomic<pid_t> processor_id{0};           // Processor 线程的 Linux TID
  std::string routine_name;
};

// ----------------------------------------------------------------------
// Processor: 工作线程封装
// ----------------------------------------------------------------------
// 职责：
//   - 持有一个 std::thread，绑定一个 ProcessorContext
//   - Run() 循环：NextRoutine() 有任务则 Resume，无任务则 Wait()
//   - 支持延迟启动：BindContext() 时通过 std::call_once 创建线程
//   - Stop() 优雅退出：通知 context Shutdown，join 线程
//
// 与原 Scheduler 的区别：
//   原 Scheduler 持有线程池 + 全局队列；Processor 是"单线程 + 单上下文"，
//   多个 Processor 组成工作窃取调度器（Step 10）。
// ----------------------------------------------------------------------
class Processor {
 public:
  Processor();
  virtual ~Processor();

  // 启动调度循环（在 thread_ 中运行）
  void Run();

  // 停止调度循环并 join 线程
  void Stop();

  // 绑定上下文并延迟启动线程（call_once 保证只启动一次）
  void BindContext(const std::shared_ptr<ProcessorContext>& context);

  // 设置本 Processor 线程可见的 Scheduler 指针（用于 Scheduler::GetThis()
  // 在工作线程中能被回调用到，如 RPC Client::HandleResponse -> NotifyTask）
  void SetScheduler(Scheduler* sched) { scheduler_ = sched; }

  // 获取底层线程句柄（供 SetSchedAffinity/SetSchedPolicy 使用）
  std::thread* Thread() { return &thread_; }

  // 获取线程 TID（自旋等待直到 TID 就绪）
  std::atomic<pid_t>& Tid();

  // 运行时快照
  std::shared_ptr<Snapshot> ProcSnapshot() { return snap_shot_; }

 private:
  std::shared_ptr<ProcessorContext> context_;
  Scheduler* scheduler_ = nullptr;

  // context_ 未绑定时的等待机制
  std::condition_variable cv_ctx_;
  std::once_flag thread_flag_;
  std::mutex mtx_ctx_;
  std::thread thread_;

  std::atomic<pid_t> tid_{-1};
  std::atomic<bool> running_{false};

  std::shared_ptr<Snapshot> snap_shot_ = std::make_shared<Snapshot>();
};

}  // namespace scheduler
}  // namespace minicyber

#endif  // MINICYBER_SCHEDULER_PROCESSOR_H_
