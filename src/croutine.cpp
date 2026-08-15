#include "minicyber/croutine/croutine.h"
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

// MainFunc 运行时，Processor 和 Scheduler 队列共同保证当前对象存活。
// 这里必须全程使用原生 CRoutine::GetCurrentRoutine 风格的裸指针：
// RoutineFactory 的数据消费循环不会返回，若在协程栈上保存 self shared_ptr，
// 每次 DATA_WAIT 都会冻结一个自引用，RemoveTask 清空外部所有权后仍无法析构。
void CRoutine::MainFunc() {
  CRoutine* cur = GetCurrentRoutine();
  cur->state_ = RoutineState::READY;  // 运行中视为 READY（CyberRT 语义）

  try {
    cur->cb_();
    cur->cb_ = nullptr;
    cur->state_ = RoutineState::FINISHED;
  } catch (...) {
    cur->cb_ = nullptr;
    cur->state_ = RoutineState::FINISHED;
  }

  cur->Yield();
}

CRoutine::CRoutine(const RoutineFunc& cb) {
  is_main_ = false;
  cb_ = cb;
  state_ = RoutineState::READY;
  // updated_ 初始化为 set 状态：使首次 UpdateState 的 test_and_set 返回 true
  // （表示无通知），避免协程刚创建就被误判为收到通知而转 READY。
  // SetUpdateFlag() 会 clear 它，使下次 test_and_set 返回 false 触发转换。
  updated_.test_and_set(std::memory_order_release);
  croutine::MakeContext(&ctx_, CRoutine::MainFunc);
}

RoutineState CRoutine::Resume() {
  if (force_stop_) {
    state_ = RoutineState::FINISHED;
    return state_;
  }

  if (state_ != RoutineState::READY) {
    return state_;
  }

  // 复用 Fiber::resume 的 shared_from_this 修复：
  // 不能用 static_cast<shared_ptr<CRoutine>>(this)，那会创建新的控制块，
  // 导致双重释放。shared_from_this() 复用原始控制块，引用计数正确。
  t_croutine = shared_from_this();
  if (!is_main_) {
    croutine::SwapContext(reinterpret_cast<char**>(&t_thread_croutine->ctx_.sp),
                          reinterpret_cast<char**>(&ctx_.sp));
  }
  return state_;
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
    croutine::SwapContext(reinterpret_cast<char**>(&cr->ctx_.sp),
                          reinterpret_cast<char**>(&t_thread_croutine->ctx_.sp));
  }
}

void CRoutine::Yield(const RoutineState& state) {
  CRoutine* cr = t_croutine.get();
  cr->state_ = state;
  Yield();
}

// ----------------------------------------------------------------------
// 协程执行所有权
// ----------------------------------------------------------------------
// 同一调度组的多个 Processor 共享就绪队列，Acquire/Release 只保证同一个
// CRoutine 不会被并发 Resume；它不表示跨队列窃取任务。
// ----------------------------------------------------------------------
bool CRoutine::Acquire() {
  return !lock_.test_and_set(std::memory_order_acquire);
}

void CRoutine::Release() {
  lock_.clear(std::memory_order_release);
}

// ----------------------------------------------------------------------
// UpdateState: 状态机更新
// ----------------------------------------------------------------------
// 1. SLEEP + 当前时间超过 wake_time_ -> READY（同步事件：超时唤醒）
// 2. updated_ 标志被 clear 过（SetUpdateFlag 被调用过）+ 状态为
//    DATA_WAIT 或 IO_WAIT -> READY（异步事件：数据/IO 就绪）
// ----------------------------------------------------------------------
// test_and_set 返回 false 表示之前是 clear 状态（即收到过通知），
// 返回 true 表示之前已是 set 状态（无新通知）。
// ----------------------------------------------------------------------
RoutineState CRoutine::UpdateState() {
  // 同步事件：SLEEP 超时
  if (state_ == RoutineState::SLEEP &&
      std::chrono::steady_clock::now() > wake_time_) {
    state_ = RoutineState::READY;
    return state_;
  }

  // 异步事件：检查 updated_ 标志
  // test_and_set 返回 false 表示之前是 clear（收到过通知）
  if (!updated_.test_and_set(std::memory_order_release)) {
    if (state_ == RoutineState::DATA_WAIT || state_ == RoutineState::IO_WAIT) {
      state_ = RoutineState::READY;
    }
  }
  return state_;
}

void CRoutine::SetUpdateFlag() {
  // clear 使下次 UpdateState 中的 test_and_set 返回 false，触发状态转换
  updated_.clear(std::memory_order_release);
}

void CRoutine::Stop() {
  force_stop_ = true;
}

void CRoutine::Wake() { state_ = RoutineState::READY; }

void CRoutine::HangUp() { Yield(RoutineState::DATA_WAIT); }

void CRoutine::Sleep(const Duration& sleep_duration) {
  wake_time_ = std::chrono::steady_clock::now() + sleep_duration;
  Yield(RoutineState::SLEEP);
}

}  // namespace minicyber
