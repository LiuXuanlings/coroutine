#ifndef MINICYBER_COMPONENT_COMPONENT_H_
#define MINICYBER_COMPONENT_COMPONENT_H_

#include <memory>
#include <type_traits>
#include <utility>

#include <google/protobuf/message.h>

#include "minicyber/base/macros.h"
#include "minicyber/common/types.h"
#include "minicyber/component/component_base.h"
#include "minicyber/croutine/routine_factory.h"
#include "minicyber/data/data_visitor.h"
#include "minicyber/scheduler/scheduler.h"

namespace minicyber {
namespace component {

// 对齐 CyberRT Component 的 reality-mode 装配职责，但终局只保留业务链需要的
// 单/双 Protobuf 输入。Reader 只建立 Hybrid 端点；DataVisitor 直接订阅输入
// Channel，RoutineFactory 只把 Proc 放入 Processor 的 CRoutine 中执行。
// 因此发布线程仅做 Dispatch/Notify，不能同步进入用户 Proc。
template <typename M0 = NullType, typename M1 = NullType>
class Component : public ComponentBase {
  static_assert(std::is_base_of<google::protobuf::Message, M0>::value,
                "Component messages must derive from google::protobuf::Message");
  static_assert(std::is_base_of<google::protobuf::Message, M1>::value,
                "Component messages must derive from google::protobuf::Message");

 public:
  Component() = default;
  ~Component() override { Shutdown(); }

  bool Initialize(const ComponentConfig& config) override {
    if (config.name().empty() || config.readers_size() < 2) {
      return false;
    }

    node_ = std::make_shared<node::Node>(config.name());
    LoadConfigFiles(config);
    if (cyber_unlikely(!Init())) {
      CleanupInitializationFailure();
      return false;
    }

    // 首输入必须先创建并作为 AllLatest 主驱动；两个 Reader 都没有同步
    // callback，接收数据只填 DataDispatcher，业务 Proc 由协程消费。
    auto primary = node_->template CreateReader<M0>(
        config.readers(0).channel(), nullptr,
        config.readers(0).pending_queue_size());
    auto secondary = node_->template CreateReader<M1>(
        config.readers(1).channel(), nullptr,
        config.readers(1).pending_queue_size());
    if (!primary || !secondary) {
      CleanupInitializationFailure();
      return false;
    }
    readers_.emplace_back(std::make_shared<ReaderT<M0>>(primary));
    readers_.emplace_back(std::make_shared<ReaderT<M1>>(secondary));

    auto visitor = std::make_shared<data::DataVisitor<M0, M1>>(
        data::VisitorConfig{primary->role_attr().channel_id(),
                            primary->PendingQueueSize()},
        data::VisitorConfig{secondary->role_attr().channel_id(),
                            secondary->PendingQueueSize()});
    auto factory = croutine::CreateRoutineFactory<M0, M1>(
        [self = std::weak_ptr<Component>(
             std::dynamic_pointer_cast<Component>(shared_from_this()))](
            const std::shared_ptr<M0>& primary_message,
            const std::shared_ptr<M1>& secondary_message) {
          if (auto component = self.lock()) {
            component->Process(primary_message, secondary_message);
          }
        },
        visitor);
    auto* scheduler = scheduler::Scheduler::GetThis();
    if (scheduler == nullptr) {
      CleanupInitializationFailure();
      return false;
    }
    const uint64_t task_id = scheduler->CreateTask(factory, config.name());
    if (task_id == 0) {
      CleanupInitializationFailure();
      return false;
    }
    AttachRoutine(scheduler, task_id, visitor);
    return true;
  }

  bool Process(const std::shared_ptr<M0>& primary,
               const std::shared_ptr<M1>& secondary) {
    if (is_shutdown_.load(std::memory_order_acquire)) {
      return true;
    }
    return Proc(primary, secondary);
  }

 private:
  virtual bool Proc(const std::shared_ptr<M0>& primary,
                    const std::shared_ptr<M1>& secondary) = 0;
};

template <typename M0>
class Component<M0, NullType> : public ComponentBase {
  static_assert(std::is_base_of<google::protobuf::Message, M0>::value,
                "Component messages must derive from google::protobuf::Message");

 public:
  Component() = default;
  ~Component() override { Shutdown(); }

  bool Initialize(const ComponentConfig& config) override {
    if (config.name().empty() || config.readers_size() < 1) {
      return false;
    }

    node_ = std::make_shared<node::Node>(config.name());
    LoadConfigFiles(config);
    if (cyber_unlikely(!Init())) {
      CleanupInitializationFailure();
      return false;
    }

    auto reader = node_->template CreateReader<M0>(
        config.readers(0).channel(), nullptr,
        config.readers(0).pending_queue_size());
    if (!reader) {
      CleanupInitializationFailure();
      return false;
    }
    readers_.emplace_back(std::make_shared<ReaderT<M0>>(reader));

    auto visitor = std::make_shared<data::DataVisitor<M0>>(
        data::VisitorConfig{reader->role_attr().channel_id(),
                            reader->PendingQueueSize()});
    auto factory = croutine::CreateRoutineFactory<M0>(
        [self = std::weak_ptr<Component>(
             std::dynamic_pointer_cast<Component>(shared_from_this()))](
            const std::shared_ptr<M0>& message) {
          if (auto component = self.lock()) {
            component->Process(message);
          }
        },
        visitor);
    auto* scheduler = scheduler::Scheduler::GetThis();
    if (scheduler == nullptr) {
      CleanupInitializationFailure();
      return false;
    }
    const uint64_t task_id = scheduler->CreateTask(factory, config.name());
    if (task_id == 0) {
      CleanupInitializationFailure();
      return false;
    }
    AttachRoutine(scheduler, task_id, visitor);
    return true;
  }

  bool Process(const std::shared_ptr<M0>& message) {
    if (is_shutdown_.load(std::memory_order_acquire)) {
      return true;
    }
    return Proc(message);
  }

 private:
  virtual bool Proc(const std::shared_ptr<M0>& message) = 0;
};

}  // namespace component
}  // namespace minicyber

#endif  // MINICYBER_COMPONENT_COMPONENT_H_
