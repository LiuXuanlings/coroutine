#include "minicyber/scheduler/scheduler.h"

#include <sched.h>
#include <unistd.h>

#include <stdexcept>

#include "minicyber/base/macros.h"
#include "minicyber/scheduler/common/pin_thread.h"
#include "minicyber/proto/scheduler_conf.pb.h"

namespace minicyber {
namespace scheduler {

thread_local Scheduler* Scheduler::t_scheduler_ = nullptr;

bool SchedulerConf::FromProto(const ::minicyber::proto::SchedulerConf& proto,
                              SchedulerConf* conf) {
  if (conf == nullptr) {
    return false;
  }

  SchedulerConf parsed;
  parsed.policy = proto.has_policy() ? proto.policy() : "classic";
  if (parsed.policy != "classic" && parsed.policy != "choreography") {
    return false;
  }
  try {
    if (parsed.policy == "classic") {
      if (!proto.has_classic_conf()) {
        return false;
      }
      for (const auto& proto_group : proto.classic_conf().groups()) {
        if (proto_group.name().empty()) {
          return false;
        }
        ClassicGroupConf group;
        group.name = proto_group.name();
        group.processor_num = proto_group.processor_num();
        group.affinity = proto_group.affinity();
        if (proto_group.has_processor_policy()) {
          group.processor_policy = proto_group.processor_policy();
        }
        group.processor_prio = proto_group.processor_prio();
        if (!proto_group.cpuset().empty()) {
          ParseCpuset(proto_group.cpuset(), &group.cpuset);
        }
        for (const auto& proto_task : proto_group.tasks()) {
          if (proto_task.name().empty()) {
            return false;
          }
          ClassicTaskConf task;
          task.name = proto_task.name();
          task.priority = proto_task.prio();
          task.group_name = group.name;
          group.tasks.push_back(std::move(task));
        }
        parsed.classic_groups.push_back(std::move(group));
      }
      if (parsed.classic_groups.empty()) {
        return false;
      }
    } else {
      if (!proto.has_choreography_conf()) {
        return false;
      }
      const auto& choreo = proto.choreography_conf();
      parsed.choreography_processor_num = choreo.choreography_processor_num();
      if (parsed.choreography_processor_num == 0 ||
          choreo.pool_processor_num() == 0) {
        return false;
      }
      parsed.choreography_affinity = choreo.choreography_affinity();
      if (choreo.has_choreography_processor_policy()) {
        parsed.choreography_processor_policy =
            choreo.choreography_processor_policy();
      }
      parsed.choreography_processor_prio =
          choreo.choreography_processor_prio();
      parsed.pool_processor_num = choreo.pool_processor_num();
      parsed.pool_affinity = choreo.pool_affinity();
      if (choreo.has_pool_processor_policy()) {
        parsed.pool_processor_policy = choreo.pool_processor_policy();
      }
      parsed.pool_processor_prio = choreo.pool_processor_prio();
      if (!choreo.choreography_cpuset().empty()) {
        ParseCpuset(choreo.choreography_cpuset(),
                    &parsed.choreography_cpuset);
      }
      if (!choreo.pool_cpuset().empty()) {
        ParseCpuset(choreo.pool_cpuset(), &parsed.pool_cpuset);
      }
      for (const auto& proto_task : choreo.tasks()) {
        if (proto_task.name().empty()) {
          return false;
        }
        ChoreographyTaskConf task;
        task.name = proto_task.name();
        task.priority = proto_task.prio();
        task.has_processor = proto_task.has_processor();
        task.processor_id = task.has_processor ? proto_task.processor() : -1;
        parsed.choreography_tasks.push_back(std::move(task));
      }
    }
  } catch (const std::invalid_argument&) {
    return false;
  }
  *conf = std::move(parsed);
  return true;
}

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
  std::lock_guard<std::mutex> lk(lifecycle_mtx_);
  return index < processors_.size() ? processors_[index]->Tid().load() : -1;
}

size_t Scheduler::ProcessorCount() const {
  std::lock_guard<std::mutex> lk(lifecycle_mtx_);
  return processors_.size();
}

void Scheduler::CreateProcessors(const SchedulerConf& conf) {
  if (policy_ == "choreography") {
    uint32_t proc_num = conf.choreography_processor_num != 0
                            ? conf.choreography_processor_num
                            : conf.thread_num;
    if (proc_num == 0) {
      proc_num = static_cast<uint32_t>(sysconf(_SC_NPROCESSORS_ONLN));
      if (proc_num == 0) proc_num = 1;
    }
    choreography_processor_count_ = proc_num;
    for (const auto& task : conf.choreography_tasks) {
      choreography_tasks_[task.name] = task;
    }

    SchedulerConf choreography_conf;
    choreography_conf.affinity = conf.choreography_affinity;
    choreography_conf.cpuset = conf.choreography_cpuset;
    choreography_conf.processor_policy = conf.choreography_processor_policy;
    choreography_conf.processor_prio = conf.choreography_processor_prio;
    for (uint32_t i = 0; i < proc_num; ++i) {
      auto ctx = std::make_shared<ChoreographyContext>();
      auto proc = std::make_shared<Processor>();
      proc->SetScheduler(this);
      proc->BindContext(ctx);
      ApplyThreadPolicy(choreography_conf, i, proc.get());
      contexts_.push_back(ctx);
      processors_.push_back(proc);
    }

    pool_processor_count_ = conf.pool_processor_num;
    SchedulerConf pool_conf;
    pool_conf.affinity = conf.pool_affinity;
    pool_conf.cpuset = conf.pool_cpuset;
    pool_conf.processor_policy = conf.pool_processor_policy;
    pool_conf.processor_prio = conf.pool_processor_prio;
    for (uint32_t i = 0; i < pool_processor_count_; ++i) {
      auto ctx = std::make_shared<ClassicContext>(DEFAULT_GROUP_NAME);
      auto proc = std::make_shared<Processor>();
      proc->SetScheduler(this);
      proc->BindContext(ctx);
      ApplyThreadPolicy(pool_conf, i, proc.get());
      contexts_.push_back(ctx);
      processors_.push_back(proc);
    }
    has_choreography_pool_ = pool_processor_count_ != 0;
    return;
  }

  classic_groups_ = conf.classic_groups;
  if (classic_groups_.empty()) {
    ClassicGroupConf group;
    group.processor_num = conf.thread_num;
    group.affinity = conf.affinity;
    group.cpuset = conf.cpuset;
    group.processor_policy = conf.processor_policy;
    group.processor_prio = conf.processor_prio;
    classic_groups_.push_back(std::move(group));
  }

  for (const auto& group : classic_groups_) {
    uint32_t proc_num = group.processor_num;
    if (proc_num == 0) {
      proc_num = static_cast<uint32_t>(sysconf(_SC_NPROCESSORS_ONLN));
      if (proc_num == 0) proc_num = 1;
    }
    for (const auto& task : group.tasks) {
      ClassicTaskConf configured = task;
      configured.group_name = group.name;
      classic_tasks_[configured.name] = std::move(configured);
    }
    for (uint32_t i = 0; i < proc_num; ++i) {
      auto ctx = std::make_shared<ClassicContext>(group.name);
      auto proc = std::make_shared<Processor>();
      proc->SetScheduler(this);
      proc->BindContext(ctx);
      SchedulerConf group_conf;
      group_conf.affinity = group.affinity;
      group_conf.cpuset = group.cpuset;
      group_conf.processor_policy = group.processor_policy;
      group_conf.processor_prio = group.processor_prio;
      ApplyThreadPolicy(group_conf, i, proc.get());
      contexts_.push_back(ctx);
      processors_.push_back(proc);
    }
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

  std::lock_guard<std::mutex> lifecycle_lk(lifecycle_mtx_);
  // Shutdown may have started after the optimistic fast-path above.
  if (stop_.load() || processors_.empty()) {
    return 0;
  }

  uint32_t target = 0;
  if (policy_ == "choreography" && processor_id >= 0 &&
      static_cast<size_t>(processor_id) < processors_.size()) {
    target = static_cast<uint32_t>(processor_id);
  } else if (policy_ == "choreography") {
    target = 0;
  }
  if (policy_ == "choreography") {
    const auto* configured = FindChoreographyTask(name);
    int requested_processor = processor_id;
    if (configured != nullptr) {
      cr->set_priority(configured->priority);
      if (configured->has_processor) {
        requested_processor = configured->processor_id;
      }
    }
    if (IsChoreographyProcessor(requested_processor)) {
      target = static_cast<uint32_t>(requested_processor);
      cr->set_processor_id(requested_processor);
    } else {
      // 未指定或越出定向区的任务统一进入 MC-610 的 Classic 公共组。
      cr->set_processor_id(-1);
      cr->set_group_name(DEFAULT_GROUP_NAME);
    }
  } else {
    cr->set_processor_id(-1);
    const auto* group = FindClassicGroup(name);
    if (group == nullptr) {
      return 0;
    }
    cr->set_group_name(group->name);
    const auto task = classic_tasks_.find(name);
    if (task != classic_tasks_.end()) {
      cr->set_priority(task->second.priority);
    }
  }

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

uint64_t Scheduler::CreateTask(const croutine::RoutineFactory& factory,
                               const std::string& name, uint32_t prio,
                               int processor_id) {
  const auto visitor = factory.GetDataVisitor();
  if (!visitor || !factory.create_routine) {
    return 0;
  }

  auto wake_state = std::make_shared<RoutineWakeState>();
  wake_state->scheduler = this;
  visitor->RegisterNotifyCallback([wake_state] {
    std::lock_guard<std::mutex> lock(wake_state->mutex);
    if (wake_state->scheduler != nullptr && wake_state->task_id != 0) {
      wake_state->scheduler->NotifyTask(wake_state->task_id);
    }
  });
  const uint64_t task_id = CreateTask(factory.CreateRoutine(), name, prio,
                                      processor_id);
  if (task_id == 0) {
    std::lock_guard<std::mutex> lock(wake_state->mutex);
    wake_state->scheduler = nullptr;
    return 0;
  }
  {
    std::lock_guard<std::mutex> lock(wake_state->mutex);
    wake_state->task_id = task_id;
  }
  {
    std::lock_guard<std::mutex> lock(routine_wake_mtx_);
    // Shutdown 可能在 CreateTask 返回后开始。只有在状态仍可用时才发布
    // Visitor 回调；否则不能把 this 留给生命周期更长的 DataVisitor。
    if (stop_.load()) {
      std::lock_guard<std::mutex> state_lock(wake_state->mutex);
      wake_state->scheduler = nullptr;
      return 0;
    }
    routine_wake_states_.push_back(std::move(wake_state));
  }
  return task_id;
}

const ClassicGroupConf* Scheduler::FindClassicGroup(
    const std::string& name) const {
  const auto task = classic_tasks_.find(name);
  const std::string& group_name =
      task == classic_tasks_.end() ? classic_groups_.front().name
                                   : task->second.group_name;
  for (const auto& group : classic_groups_) {
    if (group.name == group_name) {
      return &group;
    }
  }
  return nullptr;
}

bool Scheduler::Enqueue(const std::shared_ptr<CRoutine>& cr, uint32_t target) {
  if (policy_ == "choreography") {
    if (IsChoreographyProcessor(cr->processor_id())) {
      return std::static_pointer_cast<ChoreographyContext>(contexts_.at(target))
          ->Enqueue(cr);
    }
    if (!has_choreography_pool_) {
      return false;
    }
    ClassicContext::Enqueue(cr);
    return true;
  }
  ClassicContext::Enqueue(cr);
  return true;
}

const ChoreographyTaskConf* Scheduler::FindChoreographyTask(
    const std::string& name) const {
  const auto it = choreography_tasks_.find(name);
  return it == choreography_tasks_.end() ? nullptr : &it->second;
}

bool Scheduler::IsChoreographyProcessor(int processor_id) const {
  return policy_ == "choreography" && processor_id >= 0 &&
         static_cast<size_t>(processor_id) < choreography_processor_count_;
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

  std::lock_guard<std::mutex> lifecycle_lk(lifecycle_mtx_);
  if (stop_.load()) {
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

  if (IsChoreographyProcessor(cr->processor_id())) {
    std::static_pointer_cast<ChoreographyContext>(
        contexts_.at(static_cast<size_t>(cr->processor_id())))
        ->Notify();
  } else {
    ClassicContext::Notify(cr->group_name());
  }
  return true;
}

bool Scheduler::RemoveTask(uint64_t crid) {
  if (crid == 0) {
    return false;
  }

  std::shared_ptr<CRoutine> cr;
  {
    std::lock_guard<std::mutex> lock(id_cr_mtx_);
    const auto it = id_cr_.find(crid);
    if (it == id_cr_.end()) {
      return false;
    }
    cr = it->second;
  }

  // 先撤销 DataVisitor -> Scheduler 的唤醒闭包。这里不持有状态锁等待
  // Scheduler，避免发布线程正在回调时与组件自身关闭形成锁环。
  std::vector<std::shared_ptr<RoutineWakeState>> removed_states;
  {
    std::lock_guard<std::mutex> lock(routine_wake_mtx_);
    auto it = routine_wake_states_.begin();
    while (it != routine_wake_states_.end()) {
      if ((*it)->task_id == crid) {
        removed_states.push_back(*it);
        it = routine_wake_states_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (const auto& state : removed_states) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->scheduler = nullptr;
  }

  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mtx_);
  if (stop_.load()) {
    std::lock_guard<std::mutex> lock(id_cr_mtx_);
    id_cr_.erase(crid);
    return true;
  }

  // Stop 先禁止下一次 Resume；上下文随后从就绪队列摘除，保证队列不再
  // 持有组件协程。上下文实现会识别“当前协程就是待移除协程”的情况，
  // 因而回调内 Shutdown 不会等待自身释放 Acquire。
  cr->Stop();
  bool removed = false;
  if (IsChoreographyProcessor(cr->processor_id()) &&
      static_cast<size_t>(cr->processor_id()) < contexts_.size()) {
    removed = std::static_pointer_cast<ChoreographyContext>(
                  contexts_.at(static_cast<size_t>(cr->processor_id())))
                  ->RemoveCRoutine(crid);
  } else {
    removed = ClassicContext::RemoveCRoutine(cr);
  }
  {
    std::lock_guard<std::mutex> lock(id_cr_mtx_);
    id_cr_.erase(crid);
  }
  return removed;
}

// ----------------------------------------------------------------------
// Shutdown: 停止所有 Processor 并清理
// ----------------------------------------------------------------------
void Scheduler::Shutdown() {
  if (stop_.exchange(true)) {
    return;
  }

  // DataVisitor 可以比 Scheduler 活得更久。先让其回调无法再取得 this，
  // 再拆除任务映射和 Processor，维持 DATA_WAIT 唤醒的所有权边界。
  {
    std::lock_guard<std::mutex> states_lock(routine_wake_mtx_);
    for (const auto& state : routine_wake_states_) {
      std::lock_guard<std::mutex> state_lock(state->mutex);
      state->scheduler = nullptr;
    }
    routine_wake_states_.clear();
  }

  std::vector<std::shared_ptr<Processor>> processors;
  std::vector<std::shared_ptr<ProcessorContext>> contexts;
  {
    std::lock_guard<std::mutex> lifecycle_lk(lifecycle_mtx_);
    processors.swap(processors_);
    contexts.swap(contexts_);
  }

  // Stop contexts before joining processors so every Wait() is released.
  for (auto& context : contexts) {
    if (context) context->Shutdown();
  }
  for (auto& proc : processors) {
    if (proc) proc->Stop();
  }

  if (policy_ == "classic") {
    for (const auto& group : classic_groups_) {
      ClassicContext::RemoveGroup(group.name);
    }
  } else if (has_choreography_pool_) {
    ClassicContext::RemoveGroup(DEFAULT_GROUP_NAME);
  }

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
