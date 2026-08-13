#ifndef MINICYBER_SCHEDULER_POLICY_CHOREOGRAPHY_CONTEXT_H_
#define MINICYBER_SCHEDULER_POLICY_CHOREOGRAPHY_CONTEXT_H_

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

#include "minicyber/base/atomic_rw_lock.h"
#include "minicyber/croutine/croutine.h"
#include "minicyber/scheduler/processor_context.h"

namespace minicyber {
namespace scheduler {

// =============================================================================
// ChoreographyContext：编排调度上下文（对齐 CyberRT choreography_context）
//
// 与 ClassicContext 的核心差异：
//   - ClassicContext：多级优先级队列 + Work-Stealing。Processor 空闲时扫描
//     所有优先级队列、并从其他 group 窃取任务。复杂 DAG 中大量协程处于
//     DATA_WAIT 时，扫描开销随队列长度线性增长。
//   - ChoreographyContext：单一就绪队列，任务由上游"点对点"显式 Enqueue。
//     NextRoutine() 从最高优先级扫描本地队列，不窃取其他 Context 的任务。
//     专为点对点 DAG 依赖触发场景设计。
//
// 实例本地状态（无静态成员）：
//   - cr_queue_：multimap<prio, CRoutine, greater>，begin() 即最高优先级。
//   - rq_lk_：AtomicRWLock 保护 cr_queue_（读：NextRoutine；写：Enqueue/Remove）。
//   - mtx_wq_/cv_wq_/notify_：Wait/Notify 阻塞唤醒机制。
//
// 生命周期契约：
//   - NextRoutine 返回的协程处于 Acquired 状态，调用方执行完后须 Release。
//   - 协程执行完毕（FINISHED）后不从 cr_queue_ 自动移除，避免在热路径加锁；
//     清理由 RemoveCRoutine(crid) 显式完成。这与 CyberRT 原生语义一致。
// =============================================================================

class ChoreographyContext : public ProcessorContext {
 public:
  ChoreographyContext() = default;
  ~ChoreographyContext() override = default;

  // 取下一个就绪协程。从最高优先级开始扫描 cr_queue_，返回首个
  // Acquire 成功且 UpdateState 为 READY 的协程（调用方负责 Release）。
  // 无就绪协程时返回 nullptr。不会窃取其他 Context 的任务。
  std::shared_ptr<CRoutine> NextRoutine() override;

  // 将协程显式推入本上下文的就绪队列。由上游依赖满足后定向调用
  // （Step 37 的 Scheduler::NotifyTask -> ChoreographyContext::Enqueue）。
  // 写锁保护 multimap 插入。
  bool Enqueue(const std::shared_ptr<CRoutine>& cr);

  // 通知 Wait() 解除阻塞。notify_ 计数 +1 后 notify_one。
  void Notify();

  // 队列空时阻塞等待，直到 Notify 或 Shutdown。wait_for 1s 兜底
  // 防止计数丢失导致永久阻塞。
  void Wait() override;

  // 停止上下文：设置 stop_，notify_ 置 max，notify_all 唤醒所有 Wait。
  void Shutdown() override;

  // 按 id 从 cr_queue_ 移除协程。Stop 后自旋等待正在执行的 Processor
  // Release，再 erase。未找到返回 false。
  bool RemoveCRoutine(uint64_t crid);

 private:
  // Wait/Notify 阻塞唤醒机制
  std::mutex mtx_wq_;
  std::condition_variable cv_wq_;
  int notify_ = 0;

  // 就绪队列保护锁：NextRoutine 读锁，Enqueue/Remove 写锁
  AtomicRWLock rq_lk_;
  // 就绪队列：prio 为 key，greater 使 begin() 指向最高优先级；
  // 同优先级按插入顺序（multimap 稳定性）。
  std::multimap<uint32_t, std::shared_ptr<CRoutine>, std::greater<uint32_t>>
      cr_queue_;
};

}  // namespace scheduler
}  // namespace minicyber

#endif  // MINICYBER_SCHEDULER_POLICY_CHOREOGRAPHY_CONTEXT_H_
