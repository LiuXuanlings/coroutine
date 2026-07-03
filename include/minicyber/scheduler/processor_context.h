#ifndef MINICYBER_SCHEDULER_PROCESSOR_CONTEXT_H_
#define MINICYBER_SCHEDULER_PROCESSOR_CONTEXT_H_

#include <atomic>
#include <memory>

namespace minicyber {

class CRoutine;

namespace scheduler {

// ----------------------------------------------------------------------
// ProcessorContext: 处理器上下文抽象接口
// ----------------------------------------------------------------------
// 定义 Processor（工作线程）如何获取下一个任务、如何等待、如何关闭。
// 具体调度策略（Classic、Choreography 等）通过继承本类实现：
//   - NextRoutine(): 从内部队列取一个就绪协程，无任务返回 nullptr
//   - Wait():        队列空时阻塞等待新任务到达
//   - Shutdown():    通知 Wait() 解除阻塞并退出（默认实现设置 stop_ 标志）
//
// stop_ 暴露给子类（protected），便于 Wait() 实现中作为退出条件判断。
// ----------------------------------------------------------------------
class ProcessorContext {
 public:
  virtual ~ProcessorContext() = default;

  // 取下一个就绪协程；无任务时返回 nullptr
  virtual std::shared_ptr<CRoutine> NextRoutine() = 0;

  // 队列空时阻塞等待，直到有新任务或被 Shutdown 唤醒
  virtual void Wait() = 0;

  // 通知 Wait() 解除阻塞并准备退出。默认实现设置 stop_ 标志。
  // 子类可重写以执行额外的唤醒逻辑（如 notify condition_variable）。
  virtual void Shutdown();

 protected:
  std::atomic<bool> stop_{false};
};

}  // namespace scheduler
}  // namespace minicyber

#endif  // MINICYBER_SCHEDULER_PROCESSOR_CONTEXT_H_
