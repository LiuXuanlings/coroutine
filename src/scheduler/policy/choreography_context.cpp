#include "minicyber/scheduler/policy/choreography_context.h"

#include <chrono>
#include <limits>
#include <thread>

#include "minicyber/base/macros.h"
#include "minicyber/base/rw_lock_guard.h"

namespace minicyber {
namespace scheduler {

// ----------------------------------------------------------------------
// NextRoutine：取最高优先级的就绪协程
// ----------------------------------------------------------------------
// 1. stop_ 为 true 直接返回 nullptr
// 2. 读锁保护 cr_queue_（多 Processor 可同时读，但 ChoreographyContext
//    通常单 Processor 绑定，读锁主要为与 Enqueue/Remove 互斥）
// 3. 从 begin()（最高优先级）遍历：
//    a. Acquire 防止同一协程被多次执行
//    b. UpdateState 将 DATA_WAIT/IO_WAIT/SLEEP 转为 READY（若已收到通知）
//    c. READY 则返回（读锁 + Acquire 持有，调用方 Release）
//    d. 非 READY 则 Release 继续找
// 4. 不窃取其他 Context 任务，不自动移除 FINISHED 协程
// ----------------------------------------------------------------------
std::shared_ptr<CRoutine> ChoreographyContext::NextRoutine() {
  if (cyber_unlikely(stop_.load())) {
    return nullptr;
  }

  ReadLockGuard<AtomicRWLock> lock(rq_lk_);
  for (auto& it : cr_queue_) {
    auto& cr = it.second;
    if (!cr->Acquire()) {
      continue;  // 被其他 Processor 占用
    }

    if (cr->UpdateState() == RoutineState::READY) {
      return cr;  // 调用方执行完后 Release
    }

    cr->Release();  // 非就绪，释放执行权
  }
  return nullptr;
}

// ----------------------------------------------------------------------
// Enqueue：显式推入就绪队列（点对点定向唤醒）
// ----------------------------------------------------------------------
// 写锁保护 multimap 插入。emplace(prio, cr) 按 priority 排序插入，
// 同优先级保持插入顺序（multimap 稳定性）。
// ----------------------------------------------------------------------
bool ChoreographyContext::Enqueue(const std::shared_ptr<CRoutine>& cr) {
  if (!cr) {
    return false;
  }
  {
    WriteLockGuard<AtomicRWLock> lock(rq_lk_);
    cr_queue_.emplace(cr->priority(), cr);
  }
  Notify();  // 唤醒等待的 Processor
  return true;
}

// ----------------------------------------------------------------------
// Notify：唤醒一个阻塞在 Wait() 的 Processor
// ----------------------------------------------------------------------
void ChoreographyContext::Notify() {
  {
    std::lock_guard<std::mutex> lk(mtx_wq_);
    ++notify_;
  }
  cv_wq_.notify_one();
}

// ----------------------------------------------------------------------
// Wait：阻塞等待新任务或 Shutdown
// ----------------------------------------------------------------------
// wait_for 1s 兜底：防止 notify 计数因某种原因丢失导致永久阻塞。
// 醒来后若 notify_ > 0 则 -1（消费一次唤醒）。
// ----------------------------------------------------------------------
void ChoreographyContext::Wait() {
  std::unique_lock<std::mutex> lk(mtx_wq_);
  cv_wq_.wait_for(lk, std::chrono::seconds(1),
                  [this]() { return notify_ > 0; });
  if (notify_ > 0) {
    --notify_;
  }
}

// ----------------------------------------------------------------------
// Shutdown：停止上下文，唤醒所有阻塞的 Wait
// ----------------------------------------------------------------------
void ChoreographyContext::Shutdown() {
  stop_.store(true);
  {
    std::lock_guard<std::mutex> lk(mtx_wq_);
    notify_ = std::numeric_limits<int>::max();
  }
  cv_wq_.notify_all();
}

// ----------------------------------------------------------------------
// RemoveCRoutine：按 id 从 cr_queue_ 移除协程
// ----------------------------------------------------------------------
// 1. 写锁保护，遍历 cr_queue_ 查找 cr->id() == crid
// 2. 找到后 Stop()（标记 FINISHED，防止 NextRoutine 再选中）
// 3. 自旋 Acquire 等待正在执行的 Processor Release（避免 use-after-free）
// 4. erase 后 Release（Acquire 是为了在 erase 期间持锁，erase 后释放）
// 5. 未找到返回 false
// ----------------------------------------------------------------------
bool ChoreographyContext::RemoveCRoutine(uint64_t crid) {
  WriteLockGuard<AtomicRWLock> lock(rq_lk_);
  for (auto it = cr_queue_.begin(); it != cr_queue_.end(); ++it) {
    // 必须拷贝 shared_ptr：erase 会销毁 multimap 节点内的 shared_ptr，
    // 若用引用则 erase 后 cr 变成悬垂引用，cr->Release() 触发 UAF。
    auto cr = it->second;// ✅ 拷贝shared_ptr，引用计数+1
    if (cr->id() != crid) {
      continue;
    }
    cr->Stop();
    // 等待正在执行的 Processor Release，避免 erase 时 use-after-free
    const bool removing_current = CRoutine::GetCurrentRoutine() == cr.get();
    if (!removing_current) {
      while (!cr->Acquire()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    cr_queue_.erase(it);// ⚠️ 销毁队列中的shared_ptr副本
    if (!removing_current) {
      cr->Release();          // 依靠局部cr保持对象存活，安全调用
    }
    return true;
  }
  return false;
}

}  // namespace scheduler
}  // namespace minicyber
