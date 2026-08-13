#include "minicyber/scheduler/scheduler.h"

#include <sched.h>
#include <unistd.h>

#include "minicyber/base/macros.h"
#include "minicyber/scheduler/common/pin_thread.h"

namespace minicyber {
namespace scheduler {

thread_local Scheduler* Scheduler::t_scheduler_ = nullptr;

Scheduler::Scheduler(const SchedulerConf& conf) {
  stop_.store(false);
  if (conf.policy == "choreography") {
    policy_ = conf.policy;
  }
  CreateProcessors(conf);
  t_scheduler_ = this;
}

Scheduler::~Scheduler() { Shutdown(); }

Scheduler* Scheduler::GetThis() { return t_scheduler_; }

pid_t Scheduler::ProcessorTid(size_t index) const {
  return index < processors_.size() ? processors_[index]->Tid().load() : -1;
}

// ----------------------------------------------------------------------
// CreateProcessors: 创建 N 个 Processor，每个绑定独立 ClassicContext
// ----------------------------------------------------------------------
// 每个 Processor 的 ClassicContext 使用 group "proc_i"，拥有独立的
// 本地优先级队列。group 名唯一，避免 ClassicContext 静态 map 冲突。
// ----------------------------------------------------------------------
void Scheduler::CreateProcessors(const SchedulerConf& conf) {
  uint32_t proc_num = policy_ == "choreography" &&
                              conf.choreography_processor_num != 0
                          ? conf.choreography_processor_num
                          : conf.thread_num;
  if (proc_num == 0) {
    // 默认按 CPU 核数
    proc_num = static_cast<uint32_t>(sysconf(_SC_NPROCESSORS_ONLN));
    if (proc_num == 0) proc_num = 1;
  }

  for (uint32_t i = 0; i < proc_num; ++i) {
    std::shared_ptr<ProcessorContext> ctx;
    if (policy_ == "choreography") {
      ctx = std::make_shared<ChoreographyContext>();
    } else {
      ctx = std::make_shared<ClassicContext>("proc_" + std::to_string(i));
    }
    auto proc = std::make_shared<Processor>();
    // 让 Processor 工作线程能通过 Scheduler::GetThis() 访问到本实例，
    // 使协程内回调（如 RPC Client::HandleResponse -> NotifyTask）可见。
    proc->SetScheduler(this);
    proc->BindContext(ctx);

    ApplyThreadPolicy(conf, i, proc.get());

    contexts_.push_back(ctx);
    processors_.push_back(proc);
  }
}

// ----------------------------------------------------------------------
// ApplyThreadPolicy: 应用 CPU 亲和性与调度策略
// ----------------------------------------------------------------------
void Scheduler::ApplyThreadPolicy(const SchedulerConf& conf, size_t proc_idx,
                                   Processor* proc) {
  if (!conf.affinity.empty() && !conf.cpuset.empty()) {
    SetSchedAffinity(proc->Thread(), conf.cpuset, conf.affinity,
                     static_cast<int>(proc_idx));
  }
  SetSchedPolicy(proc->Thread(), conf.processor_policy, conf.processor_prio,
                 proc->Tid().load());
}

// ----------------------------------------------------------------------
// CreateTask: 创建任务并分发到 Processor 队列
// ----------------------------------------------------------------------
// 1. 创建 CRoutine，分配 id
  // 2. 轮询选择目标 Processor，设置 group_name
  // 3. ClassicContext::Enqueue 入队
  // 4. 记录到 id_cr_ 映射供 NotifyTask 查找
// ----------------------------------------------------------------------
uint64_t Scheduler::CreateTask(const std::function<void()>& func,
                                const std::string& name, uint32_t prio,
                                int processor_id) {
  if (cyber_unlikely(stop_.load())) {
    return 0;
  }

  uint64_t task_id = next_task_id_.fetch_add(1);
  auto cr = std::make_shared<CRoutine>(func);
  cr->set_id(task_id);
  cr->set_name(name);
  cr->set_priority(prio);

  if (processors_.empty()) {
    return 0;
  }
  uint32_t target = 0;
  if (policy_ == "choreography" && processor_id >= 0 &&
      static_cast<size_t>(processor_id) < processors_.size()) {
    target = static_cast<uint32_t>(processor_id);
  } else {
    target = next_proc_.fetch_add(1) % processors_.size();
  }
  cr->set_processor_id(policy_ == "choreography" ? static_cast<int>(target)
                                                     : -1);
  cr->set_group_name("proc_" + std::to_string(target));

  // 记录到 id_cr_ 映射
  {
    std::lock_guard<std::mutex> lk(id_cr_mtx_);
    id_cr_[task_id] = cr;
  }

  if (!Enqueue(cr, target)) {
    std::lock_guard<std::mutex> lk(id_cr_mtx_);
    id_cr_.erase(task_id);
    return 0;
  }
  return task_id;
}

bool Scheduler::Enqueue(const std::shared_ptr<CRoutine>& cr, uint32_t target) {
  if (policy_ == "choreography") {
    return std::static_pointer_cast<ChoreographyContext>(contexts_.at(target))
        ->Enqueue(cr);
  }
  ClassicContext::Enqueue(cr);
  return true;
}

// ----------------------------------------------------------------------
// NotifyTask: 通知指定任务数据已就绪
// ----------------------------------------------------------------------
// 1. 查找 CRoutine
// 2. 若处于 DATA_WAIT/IO_WAIT，SetUpdateFlag 使下次 UpdateState 转 READY
// 3. Notify 唤醒等待的 Processor
// ----------------------------------------------------------------------
bool Scheduler::NotifyTask(uint64_t crid) {
  if (cyber_unlikely(stop_.load())) {
    return false;
  }

  std::shared_ptr<CRoutine> cr;
  {
    std::lock_guard<std::mutex> lk(id_cr_mtx_);
    auto it = id_cr_.find(crid);
    if (it == id_cr_.end()) {
      return false;
    }
    cr = it->second;
  }

  auto state = cr->State();
  if (state == RoutineState::DATA_WAIT || state == RoutineState::IO_WAIT) {
    cr->SetUpdateFlag();
  }

  if (policy_ == "choreography" && cr->processor_id() >= 0 &&
      static_cast<size_t>(cr->processor_id()) < contexts_.size()) {
    std::static_pointer_cast<ChoreographyContext>(
        contexts_.at(static_cast<size_t>(cr->processor_id())))
        ->Notify();
  } else {
    ClassicContext::Notify(cr->group_name());
  }
  return true;
}

// ----------------------------------------------------------------------
// Shutdown: 停止所有 Processor 并清理
// ----------------------------------------------------------------------
void Scheduler::Shutdown() {
  if (stop_.exchange(true)) {
    return;
  }

  // 停止所有 Processor（会 join 线程）
  for (auto& proc : processors_) {
    if (proc) proc->Stop();
  }

  processors_.clear();
  contexts_.clear();

  {
    std::lock_guard<std::mutex> lk(id_cr_mtx_);
    id_cr_.clear();
  }

  if (t_scheduler_ == this) {
    t_scheduler_ = nullptr;
  }
}

}  // namespace scheduler
}  // namespace minicyber
