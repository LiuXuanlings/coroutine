#ifndef MINICYBER_SCHEDULER_SCHEDULER_H_
#define MINICYBER_SCHEDULER_SCHEDULER_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "minicyber/croutine/croutine.h"
#include "minicyber/croutine/routine_factory.h"
#include "minicyber/scheduler/policy/classic_context.h"
#include "minicyber/scheduler/policy/choreography_context.h"
#include "minicyber/scheduler/processor.h"
#include "minicyber/scheduler/scheduler_conf.h"

namespace minicyber {
namespace scheduler {

// ----------------------------------------------------------------------
// Scheduler: 顶层调度器封装
// ----------------------------------------------------------------------
// 整合 Processor 与调度策略。Classic 对齐 CyberRT SchedulerClassic：同一
// SchedGroup 的所有 Processor 绑定同一 ClassicContext 静态组并共同扫描其
// 20 级队列；Acquire 仅保护单个 CRoutine 的并发执行所有权。
// ----------------------------------------------------------------------
class Scheduler {
 public:
  explicit Scheduler(const SchedulerConf& conf);
  ~Scheduler();

  // 单例访问（thread_local，与 CyberRT 的 Instance() 对应）
  // 注意：我们的 Scheduler 是 public 构造，便于测试隔离。
  // GetThis 返回当前线程最近创建的 Scheduler 实例。
  static Scheduler* GetThis();

  // 为当前线程设置可见的 Scheduler 指针。用于让 Processor 工作线程
  // 能通过 GetThis() 访问到所属 Scheduler（构造线程之外的线程）。
  // 由 Processor::Run() 在工作线程启动时调用。
  static void SetThisForCurrentThread(Scheduler* sched) {
    t_scheduler_ = sched;
  }

  // 创建任务并分发到 Processor 队列
  //   func:  协程入口函数
  //   name:  任务名（用于 id 生成与调试）
  //   prio:  优先级 0-19
  // 返回 task_id，0 表示失败
  uint64_t CreateTask(const std::function<void()>& func,
                      const std::string& name, uint32_t prio = 0,
                      int processor_id = -1);

  // 将 MC-609 的 RoutineFactory 接入调度器。DataVisitor 回调只转换为
  // NotifyTask，不能在发布线程运行 Proc；关闭时会先解除回调到本 Scheduler
  // 的引用，避免 Visitor 生命周期长于 Scheduler 时访问已销毁对象。
  uint64_t CreateTask(const croutine::RoutineFactory& factory,
                      const std::string& name, uint32_t prio = 0,
                      int processor_id = -1);

  // 通知指定任务数据已就绪，唤醒 DATA_WAIT/IO_WAIT
  bool NotifyTask(uint64_t crid);

  // 停止所有 Processor 并清理资源
  void Shutdown();

  // 获取 Processor 数量
  size_t ProcessorCount() const;
  pid_t ProcessorTid(size_t index) const;
  bool IsStopped() const { return stop_.load(); }
  bool IsChoreography() const { return policy_ == "choreography"; }

 private:
  // 为选定策略创建 Context 与 Processor 并启动线程。
  void CreateProcessors(const SchedulerConf& conf);
  bool Enqueue(const std::shared_ptr<CRoutine>& cr, uint32_t target);
  const ClassicGroupConf* FindClassicGroup(const std::string& name) const;

  // 应用 CPU 亲和性与调度策略
  void ApplyThreadPolicy(const SchedulerConf& conf, size_t proc_idx,
                         Processor* proc);

  // Serializes public operations that touch processor/context ownership.
  // Shutdown first marks stop_, then detaches these vectors under this lock.
  mutable std::mutex lifecycle_mtx_;
  std::vector<std::shared_ptr<Processor>> processors_;
  std::vector<std::shared_ptr<ProcessorContext>> contexts_;

  // 由 DataVisitor 保存的回调状态独立于 Scheduler 对象分配。Shutdown 在
  // 销毁 Processor 前清空 scheduler 指针，并等待当前回调离开状态锁。
  struct RoutineWakeState {
    std::mutex mutex;
    Scheduler* scheduler = nullptr;
    uint64_t task_id = 0;
  };
  std::mutex routine_wake_mtx_;
  std::vector<std::shared_ptr<RoutineWakeState>> routine_wake_states_;

  // task_id -> CRoutine 映射，用于 NotifyTask 查找
  std::mutex id_cr_mtx_;
  std::unordered_map<uint64_t, std::shared_ptr<CRoutine>> id_cr_;

  // 下一个 task_id（递增）
  std::atomic<uint64_t> next_task_id_{1};
  std::unordered_map<std::string, ClassicTaskConf> classic_tasks_;
  std::vector<ClassicGroupConf> classic_groups_;
  std::string policy_ = "classic";

  std::atomic<bool> stop_{false};

  // thread_local 单例指针
  static thread_local Scheduler* t_scheduler_;
};

}  // namespace scheduler
}  // namespace minicyber

#endif  // MINICYBER_SCHEDULER_SCHEDULER_H_
