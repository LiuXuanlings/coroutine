#ifndef MINICYBER_SCHEDULER_SCHEDULER_H_
#define MINICYBER_SCHEDULER_SCHEDULER_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "minicyber/croutine/croutine.h"
#include "minicyber/scheduler/policy/classic_context.h"
#include "minicyber/scheduler/processor.h"
#include "minicyber/scheduler/scheduler_conf.h"

namespace minicyber {
namespace scheduler {

// ----------------------------------------------------------------------
// Scheduler: 顶层调度器封装
// ----------------------------------------------------------------------
// 整合 Processor + ClassicContext，提供简洁的任务调度接口。
//
// 设计：
//   - 构造时根据 SchedulerConf 创建 N 个 Processor，每个绑定独立
//     ClassicContext（group "proc_i"），拥有本地优先级队列。
//   - CreateTask 轮询分发任务到各 Processor 本地队列。
//   - NotifyTask 查找 CRoutine 并 SetUpdateFlag + Notify 唤醒
//     处于 DATA_WAIT/IO_WAIT 的协程。
//   - Shutdown 停止所有 Processor 并清理。
//
// 与旧 Scheduler（include/minicyber/scheduler.h）的区别：
//   旧 Scheduler 基于全局队列 + 条件变量；新 Scheduler 基于
//   多 Processor 本地队列 + ClassicContext 优先级调度，为后续
//   Work-Stealing 与数据驱动唤醒铺路。
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
                      const std::string& name, uint32_t prio = 0);

  // 通知指定任务数据已就绪，唤醒 DATA_WAIT/IO_WAIT
  bool NotifyTask(uint64_t crid);

  // 停止所有 Processor 并清理资源
  void Shutdown();

  // 获取 Processor 数量
  size_t ProcessorCount() const { return processors_.size(); }

 private:
  // 为每个 Processor 创建 ClassicContext 并启动线程
  void CreateProcessors(const SchedulerConf& conf);

  // 应用 CPU 亲和性与调度策略
  void ApplyThreadPolicy(const SchedulerConf& conf, size_t proc_idx,
                         Processor* proc);

  std::vector<std::shared_ptr<Processor>> processors_;
  std::vector<std::shared_ptr<ClassicContext>> contexts_;

  // task_id -> CRoutine 映射，用于 NotifyTask 查找
  std::mutex id_cr_mtx_;
  std::unordered_map<uint64_t, std::shared_ptr<CRoutine>> id_cr_;

  // 下一个 task_id（递增）
  std::atomic<uint64_t> next_task_id_{1};
  // 轮询分发的下一个 Processor 索引
  std::atomic<uint32_t> next_proc_{0};

  std::atomic<bool> stop_{false};

  // thread_local 单例指针
  static thread_local Scheduler* t_scheduler_;
};

}  // namespace scheduler
}  // namespace minicyber

#endif  // MINICYBER_SCHEDULER_SCHEDULER_H_