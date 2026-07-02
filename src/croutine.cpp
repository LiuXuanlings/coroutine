#include "minicyber/croutine/croutine.h"
#include "minicyber/context.h"
#include <exception>

namespace minicyber {

// static thread_local: DECLARE in .h, DEFINE in .cpp (no static keyword)
thread_local CRoutine::ptr CRoutine::t_croutine = nullptr;
thread_local CRoutine::ptr CRoutine::t_thread_croutine = nullptr;

CRoutine::CRoutine() {
  is_main_ = true;
  state_ = RoutineState::READY;
}

CRoutine::~CRoutine() {}

CRoutine::ptr CRoutine::GetThis() {
  if (t_croutine != nullptr) return t_croutine;
  ptr cr(new CRoutine());
  t_thread_croutine = cr;
  t_croutine = cr;
  return t_croutine;
}

/*
 * MainFunc 的 shared_ptr 生命周期管理沿用 Fiber::mainFunc 的成熟方案：
 *
 * 协程在执行完用户回调后必须最后一次切回主协程。如果直接调用
 * GetThis()->Yield()，GetThis() 会在本协程栈上生成一个临时
 * shared_ptr，由于该协程再也不会被恢复，临时对象的析构永远不会
 * 执行，导致 CRoutine 对象及其 128KB 栈永久泄漏。
 *
 * 解决方案：
 * 1. 将 shared_ptr 提取到局部变量 cur。
 * 2. 清空 cur->cb_ 释放回调闭包中可能持有的 shared_ptr。
 * 3. 提取裸指针 raw_ptr。
 * 4. cur.reset() 在 Yield 之前销毁栈上的 shared_ptr。
 * 5. 用裸指针调用 Yield 切回主协程。
 */
void CRoutine::MainFunc() {
  CRoutine::ptr cur = GetThis();
  cur->state_ = RoutineState::READY;  // 运行中视为 READY（CyberRT 语义）

  try {
    cur->cb_();
    cur->cb_ = nullptr;
    cur->state_ = RoutineState::FINISHED;
  } catch (...) {
    cur->cb_ = nullptr;
    cur->state_ = RoutineState::FINISHED;
  }

  CRoutine* raw_ptr = cur.get();
  cur.reset();
  raw_ptr->Yield();
}

CRoutine::CRoutine(const RoutineFunc& cb, int /*stack_size*/) {
  is_main_ = false;
  cb_ = cb;
  state_ = RoutineState::READY;
  MakeContext(&ctx_, CRoutine::MainFunc);
}

void CRoutine::Resume() {
  // 复用 Fiber::resume 的 shared_from_this 修复：
  // 不能用 static_cast<shared_ptr<CRoutine>>(this)，那会创建新的控制块，
  // 导致双重释放。shared_from_this() 复用原始控制块，引用计数正确。
  t_croutine = shared_from_this();
  state_ = RoutineState::READY;
  if (!is_main_) {
    SwapContext(reinterpret_cast<char**>(&t_thread_croutine->ctx_.sp),
                reinterpret_cast<char**>(&ctx_.sp));
  }
}

void CRoutine::Yield() {
  // 关键：使用裸指针而非 GetThis()（shared_ptr）。
  // 原因：Yield 会 SwapContext 冻结当前协程栈。如果在栈上创建 shared_ptr，
  // 协程被冻结后该 shared_ptr 永远不会析构，导致 CRoutine 对象永久泄漏。
  // 这与 Fiber::mainFunc 注释中描述的泄漏问题同源。
  // CyberRT 的 GetCurrentRoutine() 返回 CRoutine* 裸指针，规避了此问题。
  CRoutine* cr = t_croutine.get();
  t_croutine = t_thread_croutine;
  if (!cr->is_main_) {
    SwapContext(reinterpret_cast<char**>(&cr->ctx_.sp),
                reinterpret_cast<char**>(&t_thread_croutine->ctx_.sp));
  }
}

void CRoutine::Yield(const RoutineState& state) {
  CRoutine* cr = t_croutine.get();
  cr->state_ = state;
  Yield();
}

}  // namespace minicyber
