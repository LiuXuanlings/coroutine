#ifndef MINICYBER_SCHEDULER_POLICY_CLASSIC_CONTEXT_H_
#define MINICYBER_SCHEDULER_POLICY_CLASSIC_CONTEXT_H_

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "minicyber/croutine/croutine.h"
#include "minicyber/scheduler/processor_context.h"

namespace minicyber {
namespace scheduler {

// 最大优先级数：0-19，共 20 级。数值越大优先级越高。
static constexpr uint32_t MAX_PRIO = 20;

// 默认组名（当前所有 ClassicContext 共用同一组）
static constexpr const char* DEFAULT_GROUP_NAME = "default_grp";

// 单级优先级队列：存储该优先级上的所有协程
using CROUTINE_QUEUE = std::deque<std::shared_ptr<CRoutine>>;
// 多级优先级队列：20 级，每级一个 CROUTINE_QUEUE
using MULTI_PRIO_QUEUE = std::array<CROUTINE_QUEUE, MAX_PRIO>;
// 每级一个 mutex，保护该优先级的队列
using LOCK_QUEUE = std::array<std::mutex, MAX_PRIO>;

// ----------------------------------------------------------------------
// ClassicContext: 经典多级优先级调度上下文
// ----------------------------------------------------------------------
// 每个 Processor 绑定一个 ClassicContext 作为本地队列。
//
// 调度逻辑（NextRoutine）：
//   从最高优先级（19）到最低（0）扫描，每级队列中找首个
//   Acquire 成功且 UpdateState 后为 READY 的协程返回。
//
// 等待逻辑（Wait）：
//   使用 condition_variable + notify 计数阻塞，
//   Notify 入队新任务时计数 +1，Wait 醒来后计数 -1。
//
// 简化说明（vs CyberRT）：
//   - 用 std::mutex 替代 AtomicRWLock（Steal 频率低，mutex 足够）
//   - 当前所有实例共用 DEFAULT_GROUP_NAME，group 概念保留以备扩展
//   - 静态成员 cr_group_/rq_locks_ 按 group 组织，与 CyberRT 一致
// ----------------------------------------------------------------------
class ClassicContext : public ProcessorContext {
 public:
  ClassicContext();
  explicit ClassicContext(const std::string& group_name);

  // 从高优先级到低优先级扫描，返回首个就绪协程
  std::shared_ptr<CRoutine> NextRoutine() override;

  // 队列空时阻塞等待，直到 Notify 或 Shutdown
  void Wait() override;

  // 停止上下文，唤醒所有阻塞的 Wait
  void Shutdown() override;

  // 入队协程到对应优先级队列，并 Notify 唤醒等待的 Processor
  static void Enqueue(const std::shared_ptr<CRoutine>& cr);

  // 通知指定 group 的 Wait 醒来
  static void Notify(const std::string& group_name);

  // 按 id 从队列中移除协程
  static bool RemoveCRoutine(const std::shared_ptr<CRoutine>& cr);

 private:
  void InitGroup(const std::string& group_name);

  std::string current_grp_;

  // 指向静态 cr_group_[group] 的多级队列
  MULTI_PRIO_QUEUE* multi_pri_rq_ = nullptr;
  // 指向静态 rq_locks_[group] 的锁数组
  LOCK_QUEUE* lq_ = nullptr;

  // 静态共享状态：所有 ClassicContext 实例按 group 共享
  static std::unordered_map<std::string, MULTI_PRIO_QUEUE> cr_group_;
  static std::unordered_map<std::string, LOCK_QUEUE> rq_locks_;
  static std::unordered_map<std::string, std::mutex> mtx_wq_;
  static std::unordered_map<std::string, std::condition_variable> cv_wq_;
  static std::unordered_map<std::string, int> notify_grp_;

  // ---------------------------------------------------------------------------
  // cr_group_    : cr = CRoutine(协程对象) + group(分组) → 按分组管理的多级就绪协程队列集合
  // rq_locks_    : rq = Ready Queue(就绪队列) + locks(锁集合) → 按分组管理的多级就绪协程队列各级的互斥锁阵列
  // mtx_wq_      : mtx = mutex(互斥锁) + wq = Wait Queue(等待队列) → 按分组管理的多级就绪协程队列的互斥锁集合
  // cv_wq_       : cv = condition_variable(条件变量) + wq = Wait Queue → 按分组管理的线程阻塞唤醒用的条件变量集合
  // notify_grp_  : notify(唤醒通知) + grp = group(分组) → 按分组统计的未消费唤醒通知计数器
  // ---------------------------------------------------------------------------
};

}  // namespace scheduler
}  // namespace minicyber

#endif  // MINICYBER_SCHEDULER_POLICY_CLASSIC_CONTEXT_H_
