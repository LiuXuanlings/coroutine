#ifndef MINICYBER_CROUTINE_CROUTINE_H
#define MINICYBER_CROUTINE_CROUTINE_H

#include "minicyber/context.h"
#include <functional>
#include <memory>

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

  // 状态访问
  RoutineState State() const { return state_; }
  void SetState(const RoutineState& state) { state_ = state; }

 private:
  CRoutine();  // 主协程构造（私有，仅 GetThis 调用）
  static void MainFunc();  // 协程入口，包装用户回调

  context ctx_;
  RoutineFunc cb_;
  bool is_main_;
  RoutineState state_;
};

}  // namespace minicyber

#endif  // MINICYBER_CROUTINE_CROUTINE_H
