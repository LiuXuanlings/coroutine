#include "minicyber/scheduler/policy/classic_context.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>

#include "minicyber/base/macros.h"

namespace minicyber {
namespace scheduler {

// 静态成员定义：所有 ClassicContext 实例按 group 共享
std::unordered_map<std::string, MULTI_PRIO_QUEUE> ClassicContext::cr_group_;
std::unordered_map<std::string, LOCK_QUEUE> ClassicContext::rq_locks_;
std::unordered_map<std::string, std::mutex> ClassicContext::mtx_wq_;
std::unordered_map<std::string, std::condition_variable> ClassicContext::cv_wq_;
std::unordered_map<std::string, int> ClassicContext::notify_grp_;

ClassicContext::ClassicContext() {
  InitGroup(DEFAULT_GROUP_NAME);
}

ClassicContext::ClassicContext(const std::string& group_name) {
  InitGroup(group_name);
}

void ClassicContext::InitGroup(const std::string& group_name) {
  // 确保 group 存在于所有静态 map 中
  cr_group_[group_name];       // 默认构造 MULTI_PRIO_QUEUE
  rq_locks_[group_name];       // 默认构造 LOCK_QUEUE
  notify_grp_[group_name] = 0;

  multi_pri_rq_ = &cr_group_[group_name];
  lq_ = &rq_locks_[group_name];
  current_grp_ = group_name;
}

// ----------------------------------------------------------------------
// NextRoutine: 从高优先级到低优先级扫描本地队列，返回首个就绪协程
// ----------------------------------------------------------------------
// 1. 若 stop_ 为 true，直接返回 nullptr
// 2. 从 MAX_PRIO-1 到 0 扫描本地每级队列：
//    a. 加锁访问该级队列
//    b. 遍历协程，尝试 Acquire（防止多 Processor 同时执行同一协程）
//    c. Acquire 成功后 UpdateState，若为 READY 则返回该协程
//    d. 若非 READY，Release 锁继续找下一个
// 3. 本地全空时，遍历其他 group 尝试 Steal（Work-Stealing）
// ----------------------------------------------------------------------
std::shared_ptr<CRoutine> ClassicContext::NextRoutine() {
  if (cyber_unlikely(stop_.load())) {
    return nullptr;
  }

  // 1. 扫描本地队列
  for (int i = MAX_PRIO - 1; i >= 0; --i) {
    std::lock_guard<std::mutex> lk(lq_->at(i));
    for (auto& cr : multi_pri_rq_->at(i)) {
      if (!cr->Acquire()) {
        continue;  // 被其他 Processor 占用
      }

      if (cr->UpdateState() == RoutineState::READY) {
        return cr;  // 调用方负责执行完后 Release
      }

      cr->Release();  // 非就绪，释放锁
    }
  }

  return nullptr;
}

// ----------------------------------------------------------------------
// Wait: 阻塞等待新任务或 Shutdown
// ----------------------------------------------------------------------
// 使用 condition_variable + notify 计数实现：
//   - Notify 时 notify_grp_++ 并 notify_one
//   - Wait 醒来后若 notify_grp_ > 0 则 -1 并返回
//   - Shutdown 时 notify_grp_ 设为 max 并 notify_all
// wait_for 超时 1s 是兜底，防止 notify 计数丢失导致永久阻塞
// ----------------------------------------------------------------------
void ClassicContext::Wait() {
  std::unique_lock<std::mutex> lk(mtx_wq_[current_grp_]);
  cv_wq_[current_grp_].wait_for(
      lk, std::chrono::seconds(1),
      [&]() { return notify_grp_[current_grp_] > 0; });
  if (notify_grp_[current_grp_] > 0) {
    notify_grp_[current_grp_]--;
  }
}

// ----------------------------------------------------------------------
// Shutdown: 停止上下文，唤醒所有阻塞的 Wait
// ----------------------------------------------------------------------
void ClassicContext::Shutdown() {
  stop_.store(true);
  {
    std::lock_guard<std::mutex> lk(mtx_wq_[current_grp_]);
    notify_grp_[current_grp_] = std::numeric_limits<int>::max();
  }
  cv_wq_[current_grp_].notify_all();
}

// ----------------------------------------------------------------------
// Enqueue: 入队协程到对应优先级，并 Notify 唤醒
// ----------------------------------------------------------------------
void ClassicContext::Enqueue(const std::shared_ptr<CRoutine>& cr) {
  const std::string& grp = cr->group_name();
  uint32_t prio = cr->priority();
  if (prio >= MAX_PRIO) {
    prio = MAX_PRIO - 1;
  }

  {
    std::lock_guard<std::mutex> lk(rq_locks_[grp].at(prio));
    cr_group_[grp].at(prio).push_back(cr);
  }

  Notify(grp);
}

// ----------------------------------------------------------------------
// Notify: 通知指定 group 的 Wait 醒来
// ----------------------------------------------------------------------
void ClassicContext::Notify(const std::string& group_name) {
  {
    std::lock_guard<std::mutex> lk(mtx_wq_[group_name]);
    notify_grp_[group_name]++;
  }
  cv_wq_[group_name].notify_one();
}

// ----------------------------------------------------------------------
// RemoveCRoutine: 按 id 从队列中移除协程
// ----------------------------------------------------------------------
bool ClassicContext::RemoveCRoutine(const std::shared_ptr<CRoutine>& cr) {
  const std::string& grp = cr->group_name();
  uint32_t prio = cr->priority();
  if (prio >= MAX_PRIO) {
    prio = MAX_PRIO - 1;
  }

  std::lock_guard<std::mutex> lk(rq_locks_[grp].at(prio));
  auto& queue = cr_group_[grp].at(prio);
  auto it = std::find_if(queue.begin(), queue.end(),
                         [&](const std::shared_ptr<CRoutine>& c) {
                           return c->id() == cr->id();
                         });
  if (it == queue.end()) {
    return false;
  }
  auto target = *it;
  target->Stop();
  while (!target->Acquire()) {
    std::this_thread::sleep_for(std::chrono::microseconds(1));
  }
  queue.erase(it);
  target->Release();
  return true;
}

}  // namespace scheduler
}  // namespace minicyber
