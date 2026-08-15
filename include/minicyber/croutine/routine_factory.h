#ifndef MINICYBER_CROUTINE_ROUTINE_FACTORY_H_
#define MINICYBER_CROUTINE_ROUTINE_FACTORY_H_

#include <functional>
#include <memory>
#include <utility>

#include "minicyber/croutine/croutine.h"
#include "minicyber/data/data_visitor.h"
#include "minicyber/data/data_visitor_base.h"

namespace minicyber {
namespace croutine {

// RoutineFactory 对齐 CyberRT 的数据协程工厂：把 DataVisitorBase 交给调度层
// 注册 NotifyTask 回调，同时为每个 CRoutine 创建独立的无限消费循环。它只支持
// 单/双输入，且不持有 Scheduler，使数据消费与调度策略解耦。
class RoutineFactory {
 public:
  using RoutineFunc = CRoutine::RoutineFunc;
  using CreateRoutineFunc = std::function<RoutineFunc()>;

  RoutineFunc CreateRoutine() const {
    return create_routine ? create_routine() : RoutineFunc{};
  }

  std::shared_ptr<data::DataVisitorBase> GetDataVisitor() const {
    return data_visitor_;
  }

  // 与原生 RoutineFactory 一样，调度层可取出此生成器为每个任务创建协程函数。
  CreateRoutineFunc create_routine;

 private:
  template <typename M0, typename F>
  friend RoutineFactory CreateRoutineFactory(
      F&& procedure, const std::shared_ptr<data::DataVisitor<M0>>& visitor);
  template <typename M0, typename M1, typename F>
  friend RoutineFactory CreateRoutineFactory(
      F&& procedure, const std::shared_ptr<data::DataVisitor<M0, M1>>& visitor);

  void SetDataVisitor(const std::shared_ptr<data::DataVisitorBase>& visitor) {
    data_visitor_ = visitor;
  }

  std::shared_ptr<data::DataVisitorBase> data_visitor_;
};

template <typename M0, typename F>
RoutineFactory CreateRoutineFactory(
    F&& procedure, const std::shared_ptr<data::DataVisitor<M0>>& visitor) {
  RoutineFactory factory;
  factory.SetDataVisitor(visitor);
  factory.create_routine = [visitor,
                            procedure = std::forward<F>(procedure)]() mutable {
    return [visitor, procedure]() mutable {
      std::shared_ptr<M0> message;
      for (;;) {
        // 先声明等待，再检查数据；DataNotifier 在 Dispatch 后调用调度层回调，
        // 由后者把 DATA_WAIT 协程转为 READY，避免发布线程直接执行业务 Proc。
        CRoutine::GetCurrentRoutine()->SetState(RoutineState::DATA_WAIT);
        if (visitor->TryFetch(message)) {
          procedure(message);
          // 协程栈在 DATA_WAIT 时会被冻结，任务被移除不会展开该栈；让出前
          // 必须释放最后一条消息，否则其 shared_ptr 会随挂起栈一起泄漏。
          message.reset();
          CRoutine::Yield(RoutineState::READY);
        } else {
          CRoutine::Yield();
        }
      }
    };
  };
  return factory;
}

template <typename M0, typename M1, typename F>
RoutineFactory CreateRoutineFactory(
    F&& procedure, const std::shared_ptr<data::DataVisitor<M0, M1>>& visitor) {
  RoutineFactory factory;
  factory.SetDataVisitor(visitor);
  factory.create_routine = [visitor,
                            procedure = std::forward<F>(procedure)]() mutable {
    return [visitor, procedure]() mutable {
      std::shared_ptr<M0> primary_message;
      std::shared_ptr<M1> secondary_message;
      for (;;) {
        CRoutine::GetCurrentRoutine()->SetState(RoutineState::DATA_WAIT);
        if (visitor->TryFetch(primary_message, secondary_message)) {
          procedure(primary_message, secondary_message);
          primary_message.reset();
          secondary_message.reset();
          CRoutine::Yield(RoutineState::READY);
        } else {
          CRoutine::Yield();
        }
      }
    };
  };
  return factory;
}

}  // namespace croutine
}  // namespace minicyber

#endif  // MINICYBER_CROUTINE_ROUTINE_FACTORY_H_
