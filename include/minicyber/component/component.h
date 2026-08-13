#ifndef MINICYBER_COMPONENT_COMPONENT_H_
#define MINICYBER_COMPONENT_COMPONENT_H_

#include <memory>
#include <utility>

#include "minicyber/base/macros.h"
#include "minicyber/common/types.h"
#include "minicyber/component/component_base.h"
#include "minicyber/node/node.h"
#include "minicyber/proto/component_conf.pb.h"

// =============================================================================
// MiniCyber Component Framework — Component<T> 事件驱动组件模板
//
// 设计目标：对齐 Apollo CyberRT 的 Component<M0, M1, M2, M3>，向业务开发者
//   提供 "继承 + 重写 Proc()" 的声明式编程模型。开发者只需关注业务逻辑，
//   框架自动完成：Node 创建、Reader 注册、回调绑定、生命周期管理。
//
// 使用方式：
//   class MyComponent : public minicyber::component::Component<std::string> {
//    protected:
//     bool Init() override { AINFO << "init"; return true; }
//     bool Proc(const std::shared_ptr<std::string>& msg) override {
//       AINFO << "got: " << *msg;
//       return true;
//     }
//   };
//   MINICYBER_REGISTER_COMPONENT(MyComponent)
//
// 模板偏特化架构（CyberRT 风格）：
//   Component<M0, M1, M2, M3>     ← 主模板（4 通道，声明 Process/Proc 接口）
//     ├── Component<NullType>      ← 单通道（当前 Step 实现）
//     ├── Component<M0, M1>        ← 双通道融合（Phase 7）
//     └── Component<NullType*>     ← 无输入（纯 Init，如无 Reader 启动器）
//
// 命名空间：minicyber::component，与 ComponentBase 一致
// =============================================================================

namespace minicyber {
namespace component {

// =============================================================================
// 主模板：Component<M0, M1, M2, M3>
// =============================================================================
// 主模板本身只声明接口（Process + Proc 纯虚），不提供实现。
// 真正的初始化逻辑在偏特化中完成。
//
// 注意：与 CyberRT 不同的是，MiniCyber 的 Component 主模板的 Initialize
//   默认返回 false。只有当使用到有实现的偏特化时（如 <M0>），Initialize
//   才生效。这避免了误用未完全实现的模板变体。
// =============================================================================
template <typename M0 = NullType, typename M1 = NullType,
          typename M2 = NullType, typename M3 = NullType>
class Component : public ComponentBase {
 public:
  Component() = default;
  ~Component() override { Shutdown(); }

  /**
   * @brief 初始化组件（主模板默认实现 — 始终返回 false）
   *
   * 主模板不包含真正的初始化逻辑。只有偏特化版本（单通道、双通道等）
   * 才有对应的 Initialize 实现。
   */
  bool Initialize(const ComponentConfig& config) override {
    (void)config;
    return false;
  }

  /**
   * @brief 多通道消息处理入口
   *
   * Process 作为 Proc 的非虚转发层（NVI: Non-Virtual Interface），
   * 在调用 Proc 前统一检查 is_shutdown_ 状态。
   * 这种模式与 CyberRT 一致：Process 是 public 非虚，Proc 是 private 纯虚。
   */
  bool Process(const std::shared_ptr<M0>& msg0, const std::shared_ptr<M1>& msg1,
               const std::shared_ptr<M2>& msg2,
               const std::shared_ptr<M3>& msg3) {
    (void)msg0;
    (void)msg1;
    (void)msg2;
    (void)msg3;
    return true;
  }

 private:
  virtual bool Proc(const std::shared_ptr<M0>& msg0,
                    const std::shared_ptr<M1>& msg1,
                    const std::shared_ptr<M2>& msg2,
                    const std::shared_ptr<M3>& msg3) = 0;
};

// =============================================================================
// 偏特化：Component<NullType, NullType, NullType, NullType>（无输入组件）
// =============================================================================
// 这种变体不订阅任何 Channel，仅执行 Init()。
// 适用场景：纯 Timer 组件（配合 TimerComponent）或仅用作启动器。
//
// 与 CyberRT 的差异：
//   CyberRT 将这个变体用于 "Component 基类" 语义（所有 Component 变体
//   都继承自此）。MiniCyber 将其作为偏特化存在，因为 MiniCyber 的
//   ComponentBase 已经承担了基类角色。
// =============================================================================
template <>
class Component<NullType, NullType, NullType, NullType> : public ComponentBase {
 public:
  Component() = default;
  ~Component() override { Shutdown(); }

  /**
   * @brief 无输入组件的初始化：创建 Node + Init()
   *
   * 创建 Node 用于拓扑注册（mainboard 可以感知到这个节点的存在），
   * 然后调用 Init() 由派生类完成具体初始化逻辑。
   */
  bool Initialize(const ComponentConfig& config) override;
};

// =============================================================================
// 偏特化：Component<M0, NullType, NullType, NullType>（单通道组件）
// =============================================================================
// 这是最常用的变体——组件订阅一个 Channel，每条到达的消息触发一次 Proc()。
//
// Initialize 执行流程：
//   1. node_.reset(new Node(config.name()))        — 创建 Node
//   2. LoadConfigFiles(config)                       — 记录配置路径
//   3. config.readers_size() >= 1 检查               — 必须至少一个 Reader
//   4. Init()                                        — 虚函数，业务初始化
//   5. 创建 Reader<M0> 并绑定 Process 回调           — 注册数据通道
//   6. 将 Reader 存入 readers_ 向量                   — 生命周期管理
//
// 回调传递路径：
//   Writer::Write(msg)
//     → IntraTransmitter::Transmit(msg)
//       → IntraDispatcher<M0>::Dispatch(channel_id, msg)
//         → DataDispatcher<M0>::Dispatch(channel_id, msg)
//           → ChannelBuffer<M0>::Fill(msg)
//           → DataNotifier::Notify(channel_id)
//             → [Reader 回调] Component<M0>::Process(msg)
//               → Proc(msg)
//
// 注意：这个回调链在 Writer 线程中**同步执行**，没有跨线程切换。
// 这是 MiniCyber 当前阶段的简化（Phase 7 引入协程任务后，
// 回调会变为唤醒协程后在 Processor 线程中异步执行）。
// =============================================================================
template <typename M0>
class Component<M0, NullType, NullType, NullType> : public ComponentBase {
 public:
  Component() = default;
  ~Component() override { Shutdown(); }

  /**
   * @brief 单通道组件的初始化
   *
   * @param config 组件配置（至少包含一个 ReaderOption 定义 channel 名称）
   * @return true  初始化成功
   * @return false 初始化失败（无 Reader 配置、Init() 失败、创建 Reader 失败）
   */
  bool Initialize(const ComponentConfig& config) override;

  /**
   * @brief 消息处理入口（非虚转发层）
   *
   * NVI 模式：Process 检查 is_shutdown_ 后转发到纯虚 Proc()。
   * 这个设计让框架能在 Proc 前后插入通用逻辑（如统计、日志），
   * 而无需业务代码做任何改动。
   *
   * @param msg 从 Channel 到达的消息
   * @return true  处理成功
   * @return false 处理失败（Proc 返回 false）
   */
  bool Process(const std::shared_ptr<M0>& msg);

 private:
  /**
   * @brief 业务处理回调（纯虚，由业务组件重写）
   *
   * 这是组件开发者唯一需要关心的函数。
   * 每条到达的消息触发一次调用，运行在发布者线程中（当前阶段）。
   *
   * @param msg 从 Channel 到达的消息
   * @return true  处理成功
   * @return false 处理失败
   */
  virtual bool Proc(const std::shared_ptr<M0>& msg) = 0;
};

// =============================================================================
// 实现：无输入组件 Initialize
// =============================================================================
inline bool Component<NullType, NullType, NullType>::Initialize(
    const ComponentConfig& config) {
  // 创建 Node — 即使无输入也注册到 TopologyManager，便于框架感知
  node_.reset(new node::Node(config.name()));
  LoadConfigFiles(config);

  if (cyber_unlikely(!Init())) {
    CleanupInitializationFailure();
    return false;
  }
  return true;
}

// =============================================================================
// 实现：单通道组件 Initialize
// =============================================================================
template <typename M0>
bool Component<M0, NullType, NullType, NullType>::Initialize(
    const ComponentConfig& config) {
  // Step 1: 创建 Node（自动向 TopologyManager 注册节点名）
  node_.reset(new node::Node(config.name()));
  LoadConfigFiles(config);

  // Step 2: 校验配置 — 单通道组件必须至少有一个 Reader 配置
  if (cyber_unlikely(config.readers_size() < 1)) {
    CleanupInitializationFailure();
    return false;
  }

  // Step 3: 业务初始化（由派生类重写）
  if (cyber_unlikely(!Init())) {
    CleanupInitializationFailure();
    return false;
  }

  // Step 4: 构建回调闭包
  // 使用 weak_ptr 防止组件析构后回调仍被调用（悬垂引用）。
  // std::dynamic_pointer_cast 将 shared_from_this() 从 ComponentBase 转为
  // Component<M0> 类型。这是必须的，因为回调需要绑定到 Process(msg)，
  // 而 Process 是 Component<M0> 的成员，不是 ComponentBase 的。
  //
  // 为什么不用 shared_from_this() 直接绑？
  // 如果回调持有 shared_ptr（而非 weak_ptr），会形成循环引用：
  //   Component → Reader → DataNotifier → callback → shared_ptr → Component
  // 这导致 Component 永远无法析构（内存泄漏）。
  // weak_ptr 打破了这个循环：组件正常 Shutdown → readers_.clear() →
  // Reader 析构 → DataNotifier 回调条目移除 → weak_ptr 到期 → 安全。
  std::weak_ptr<Component<M0>> self =
      std::dynamic_pointer_cast<Component<M0>>(shared_from_this());
  auto func = [self](const std::shared_ptr<M0>& msg) {
    auto ptr = self.lock();
    if (cyber_likely(ptr != nullptr)) {
      ptr->Process(msg);
    }
  };

  // Step 5: 创建 Reader（通过 Node 工厂方法）
  // CreateReader 内部调用 Transport::CreateReceiver<T>，自动决策
  // IntraReceiver 或 ShmReceiver（基于 TopologyManager 判断）。
  // 回调 func 在数据到达时被同步调用（见上方回调传递路径）。
  auto reader = node_->template CreateReader<M0>(
      config.readers(0).channel(), func,
      config.readers(0).pending_queue_size());
  if (cyber_unlikely(reader == nullptr)) {
    CleanupInitializationFailure();
    return false;
  }

  // Step 6: 将 Reader 包装为 ReaderT<M0> 存入 readers_ 向量
  // ReaderT 是 ReaderBase 的派生模板，提供类型安全的擦除存储。
  // Shutdown 时 ComponentBase 遍历 readers_ 调用 ReaderBase::Shutdown()，
  // 无需知道具体的模板类型 T。
  readers_.emplace_back(std::make_shared<ReaderT<M0>>(reader));
  return true;
}

// =============================================================================
// 实现：单通道组件 Process
// =============================================================================
template <typename M0>
bool Component<M0, NullType, NullType, NullType>::Process(
    const std::shared_ptr<M0>& msg) {
  // is_shutdown_ 检查：组件已关闭时忽略后续消息。
  // 使用 memory_order_relaxed 因为 Shutdown 和 Process 之间没有
  // 严格的数据依赖关系（即使漏判一次，也只是多处理一条消息）。
  if (is_shutdown_.load(std::memory_order_relaxed)) {
    return true;
  }
  return Proc(msg);
}

}  // namespace component
}  // namespace minicyber

#endif  // MINICYBER_COMPONENT_COMPONENT_H_
