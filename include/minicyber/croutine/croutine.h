#ifndef MINICYBER_CROUTINE_CROUTINE_H
#define MINICYBER_CROUTINE_CROUTINE_H

#include "minicyber/context.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace minicyber {

// CyberRT 风格的协程状态机
// 与原 Fiber::state 的映射关系：
//   INIT      -> READY
//   EXEC      -> (内部使用，不对外)
//   HOLD      -> READY / SLEEP / IO_WAIT / DATA_WAIT (语义细化)
//   TERM      -> FINISHED
//   EXCEPT    -> (保留，由实现内部处理)
enum class RoutineState {
  READY = 0,
  FINISHED,
  SLEEP,
  IO_WAIT,
  DATA_WAIT,
};

class CRoutine : public std::enable_shared_from_this<CRoutine> {
 public:
  using ptr = std::shared_ptr<CRoutine>;
  using RoutineFunc = std::function<void()>;

  static thread_local ptr t_croutine;        // 当前正在运行的协程
  static thread_local ptr t_thread_croutine; // 当前线程的主协程

  explicit CRoutine(const RoutineFunc& cb, int stack_size = FIBER_STACK_SIZE);
  ~CRoutine();

  // 获取当前协程（若不存在则创建主协程）
  static ptr GetThis();

  // 让出执行权，切回主协程（保留当前状态）
  static void Yield();

  // 让出执行权，并设置目标状态（如 DATA_WAIT 表示因数据未就绪而挂起）
  static void Yield(const RoutineState& state);

  // 恢复执行权，从主协程切到本协程
  void Resume();

  // ----------------------------------------------------------------------
  // 工作窃取锁（Work-Stealing Lock）
  // ----------------------------------------------------------------------
  // Acquire() 尝试获取执行权，返回 true 表示成功。
  // 用于 ClassicContext::NextRoutine() 防止同一协程被多个 Processor 同时执行。
  // Processor 执行完后必须调用 Release() 释放锁。
  // ----------------------------------------------------------------------
  bool Acquire();
  void Release();

  // ----------------------------------------------------------------------
  // 状态更新机制
  // ----------------------------------------------------------------------
  // UpdateState(): 检查 SLEEP 是否超时、是否收到异步通知，
  //                将 DATA_WAIT/IO_WAIT/SLEEP 转为 READY。返回当前状态。
  // SetUpdateFlag(): 通知协程数据已就绪（由 DataNotifier 调用），
  //                  清除 updated_ 标志，使下次 UpdateState 转为 READY。
  // ----------------------------------------------------------------------
  RoutineState UpdateState();
  void SetUpdateFlag();

  // 强制停止协程（设置 force_stop_ 并标记 FINISHED）
  void Stop();

  // 状态访问
  RoutineState State() const { return state_; }
  void SetState(const RoutineState& state) { state_ = state; }

  // ----------------------------------------------------------------------
  // 元数据 getter/setter
  // ----------------------------------------------------------------------
  uint64_t id() const { return id_; }
  void set_id(uint64_t id) { id_ = id; }

  const std::string& name() const { return name_; }
  void set_name(const std::string& name) { name_ = name; }

  uint32_t priority() const { return priority_; }
  void set_priority(uint32_t priority) { priority_ = priority; }

  const std::string& group_name() const { return group_name_; }
  void set_group_name(const std::string& group) { group_name_ = group; }

  // SLEEP 状态的唤醒时间点（用于 UpdateState 判断是否超时）
  std::chrono::steady_clock::time_point wake_time() const { return wake_time_; }
  void set_wake_time(std::chrono::steady_clock::time_point t) { wake_time_ = t; }

 private:
  CRoutine();  // 主协程构造（私有，仅 GetThis 调用）
  static void MainFunc();  // 协程入口，包装用户回调

  context ctx_;
  RoutineFunc cb_;
  bool is_main_;
  RoutineState state_;

  // 工作窃取锁：test_and_set 返回 true 表示已被占用
  std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
  // 异步通知标志：clear 后 UpdateState 会将 DATA_WAIT/IO_WAIT 转为 READY
  std::atomic_flag updated_ = ATOMIC_FLAG_INIT;
  bool force_stop_ = false;

  // 元数据
  uint64_t id_ = 0;
  std::string name_;
  uint32_t priority_ = 0;
  std::string group_name_;

  // SLEEP 唤醒时间点
  std::chrono::steady_clock::time_point wake_time_ =
      std::chrono::steady_clock::now();
};

}  // namespace minicyber

#endif  // MINICYBER_CROUTINE_CROUTINE_H

