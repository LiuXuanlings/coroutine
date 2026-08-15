#ifndef MINICYBER_COMPONENT_COMPONENT_BASE_H_
#define MINICYBER_COMPONENT_COMPONENT_BASE_H_

#include <atomic>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/text_format.h>

#include "minicyber/data/data_visitor_base.h"
#include "minicyber/proto/component_conf.pb.h"
#include "minicyber/node/node.h"
#include "minicyber/scheduler/scheduler.h"

// =============================================================================
// MiniCyber Component Framework — ComponentBase 生命周期抽象
//
// 设计目标：对齐 Apollo CyberRT 的 ComponentBase，为业务组件提供规范化的
//   Initialize / Shutdown 生命周期。业务开发者只需继承 Component<T> 并重写
//   Init() 和 Proc(), 框架自动管理 Node 创建、Reader 注册和资源释放。
//
// 与 CyberRT 的裁剪边界：去掉 gflags、class_loader 和 WorkRoot；保留
// Component 任务由 Scheduler 持有的生命周期，并将 RemoveTask 放在 Reader
// 注销之前，确保 DATA_WAIT 唤醒不会再触达已关闭组件。
//
// 命名空间：minicyber::component 与 node / scheduler / data 层隔离
// =============================================================================

namespace minicyber {
namespace component {

// =============================================================================
// ReaderBase / ReaderT — 类型擦除的 Reader 包装器
// =============================================================================
// ComponentBase 需要持有一组异构的 Reader<M0>, Reader<M1>, ... 指针，以便在
// Shutdown 时统一调用 reader->Shutdown()。C++ 模板不支持直接 vector 存储
// 不同类型的 Reader<T>，因此使用「类型擦除」手法：
//
//   ReaderBase  (抽象基类, 纯虚 Shutdown)
//       ↑
//   ReaderT<T>  (派生模板, 持有 shared_ptr<node::Reader<T>>)
//
// 这种方案相比 std::any / void* 的优势：类型安全 + 零运行时开销（虚函数表
// 的代价可忽略，因为 Shutdown 调用频率极低）。
//
// Tradeoff: 每个 ReaderT 对象多一次虚函数间接调用（Shutdown 时）。
// 由于 Shutdown 在框架关闭时只调用一次，这个成本完全可接受。
//
// CyberRT 原版在 cyber/node/reader_base.h 中定义了 ReaderBase 基类，
// 所有 Reader<M> 都继承它。MiniCyber 为了保持 node 层的简洁性（Reader 没有
// 继承体系），在 component 层引入这套包装器。这是「关注点分离」的体现：
//   - node::Reader<T>: 纯数据通道接口，不做生命周期管理
//   - component::ReaderBase: 生命周期管理接口，用于框架层面统一清理
// =============================================================================
class ReaderBase {
 public:
  virtual ~ReaderBase() = default;
  virtual void Shutdown() = 0;
};

template <typename T>
class ReaderT : public ReaderBase {
 public:
  explicit ReaderT(std::shared_ptr<node::Reader<T>> reader)
      : reader_(std::move(reader)) {}

  /**
   * @brief 关闭底层 Reader 并释放 shared_ptr
   *
   * Shutdown 幂等：reader_ 置空后再次调用无效果。
   * 注意这里必须 reset() 而不是仅调用 reader_->Shutdown()，因为
   * ComponentBase 的析构函数不会遍历 readers_（通用基类不知道 T），
   * 主动 reset 可以提前释放资源。
   */
  void Shutdown() override {
    if (reader_) {
      reader_->Shutdown();
      reader_.reset();
    }
  }

 private:
  std::shared_ptr<node::Reader<T>> reader_;
};

// 前置声明
using minicyber::proto::ComponentConfig;

/**
 * @brief 组件生命周期基类（抽象接口）
 *
 * 继承层次：
 *   ComponentBase                    ← 生命周期、Node 引用、配置路径
 *     ├── Component<M0>              ← 单通道事件驱动组件
 *     └── Component<M0, M1>          ← 双通道融合组件
 *
 * 典型使用流程（由 mainboard 的 ModuleController 驱动）：
 *   1. dlopen 加载 .so → 静态注册 MINICYBER_REGISTER_COMPONENT 宏
 *   2. ComponentFactory::Create() → 通过类名字符串反射构造对象
 *   3. component->Initialize(config) → 创建 Node + Reader(s) + Init()
 *   4. 框架运行（数据到达 → Proc 回调）
 *   5. 收到 SIGINT → component->Shutdown() → 清理资源
 */
class ComponentBase : public std::enable_shared_from_this<ComponentBase> {
 public:
  virtual ~ComponentBase() = default;

  // ==========================================================================
  // Initialize — 组件初始化入口（由框架调用）
  // ==========================================================================
  // 基类默认实现返回 false，表示"未实现"。真正的初始化逻辑由
  // Component<M0>::Initialize() 等事件组件重写。
  // ==========================================================================

  virtual bool Initialize(const ComponentConfig& config) { (void)config; return false; }

  /**
   * @brief 组件关闭（幂等）
   *
   * 关闭顺序严格遵守原生 ComponentBase 的所有权边界：先禁止新的业务处理，
   * 再移除 Scheduler 任务和 DataVisitor 唤醒闭包，随后注销 Reader/Transport，
   * 最后调用 Clear 释放业务 Writer，并关闭 Node。RemoveTask 对当前 Proc 的
   * 自关闭不会等待自身持有的协程执行权，避免回调线程形成等待环。
   */
  virtual void Shutdown() {
    if (is_shutdown_.exchange(true, std::memory_order_acq_rel)) {
      return;  // 第二次调用直接跳过
    }
    StopRoutine();

    for (auto& reader : readers_) {
      reader->Shutdown();
    }
    readers_.clear();

    Clear();
    if (node_) {
      node_->Shutdown();
      node_.reset();
    }
  }

  /**
   * @brief 检查组件是否已关闭
   *
   * 用于外部判断（例如 mainboard 的 WaitForShutdown）。
   * memory_order_acquire 确保能看到 Shutdown() 中所有先序操作的结果。
   */
  bool IsShutdown() const {
    return is_shutdown_.load(std::memory_order_acquire);
  }

  /**
   * @brief 从文本配置文件解析 Protobuf 对象
   *
   * 保留 cyber_ref ComponentBase 的文本 Protobuf 配置职责。
   * 空指针、空路径、文件不存在或 TextFormat 解析失败均返回 false。
   */
  template <typename T>
  bool GetProtoConfig(T* config) const {
    if (config == nullptr || config_file_path_.empty()) return false;
    std::ifstream input(config_file_path_);
    if (!input.is_open()) return false;
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    return google::protobuf::TextFormat::ParseFromString(content, config);
  }

  // 组件名称获取（来自 LoadConfigFiles 中存储的配置路径）
  const std::string& ConfigFilePath() const { return config_file_path_; }

 protected:
  // ==========================================================================
  // Init / Clear — 子类必须重写的业务钩子
  // ==========================================================================
  // Init() 是纯虚函数，所有组件必须实现初始化逻辑。
  //   典型实现：初始化内部状态、分配资源、打开文件等。
  //   返回 false 表示初始化失败，框架将跳过该组件的加载。
  //
  // Clear() 是虚函数，默认空实现。组件如有特殊清理需求（如关闭文件描述符、
  //   释放 GPU 显存）可以重写它。Clear 在 Shutdown 时被调用。
  //   注意：Clear 调用时 Scheduler 任务和 Reader 已注销，派生类
  //   应只释放自身 Writer 及业务资源，不得再依赖输入端点。
  // ==========================================================================
  virtual bool Init() = 0;
  virtual void Clear() {}

  /**
   * @brief 加载事件驱动组件的配置文件路径
   *
   * 从 ComponentConfig 中提取 config_file_path 和 flag_file_path。
   *
   * 与 CyberRT 的差异：
   *   CyberRT 原版使用 common::GetAbsolutePath(common::WorkRoot(), path)
   *   将相对路径转为绝对路径（基于 CyberRT 安装目录 WORK_ROOT）。
   *   MiniCyber 暂不提供 WorkRoot 概念，直接使用 proto 中的原始路径。
   *   如果路径以 '/' 开头，视为绝对路径直接使用；否则视为相对路径
   *   （相对于进程工作目录）。框架不强转。
   *
   * flag_file_path 的处理：
   *   CyberRT 通过 gflags 的 google::SetCommandLineOption("flagfile", path)
   *   加载命令行标志。MiniCyber 不依赖 gflags，因此跳过此处理。
   *   如果需要，上层可以自行解析 flags。
   */
  void LoadConfigFiles(const ComponentConfig& config) {
    if (!config.config_file_path().empty()) {
      config_file_path_ = config.config_file_path();
    }
  }

  // Component 持有 RoutineFactory 的 DataVisitor，直到任务移除完成；否则
  // 发布线程可能在 Reader 关闭期间仍通过 notifier 触达已销毁的 Scheduler。
  void AttachRoutine(scheduler::Scheduler* scheduler, uint64_t task_id,
                     std::shared_ptr<data::DataVisitorBase> visitor) {
    scheduler_ = scheduler;
    scheduler_task_id_ = task_id;
    data_visitor_ = std::move(visitor);
  }

  // Initialize 失败时回收已经创建的本地端点，但保留对象可重试初始化。
  void CleanupInitializationFailure() {
    StopRoutine();
    for (auto& reader : readers_) reader->Shutdown();
    readers_.clear();
    Clear();
    if (node_) {
      node_->Shutdown();
      node_.reset();
    }
  }

 private:
  void StopRoutine() {
    if (scheduler_ != nullptr && scheduler_task_id_ != 0) {
      scheduler_->RemoveTask(scheduler_task_id_);
    }
    scheduler_ = nullptr;
    scheduler_task_id_ = 0;
    data_visitor_.reset();
  }

 protected:

  // ==========================================================================
  // 成员变量
  // ==========================================================================

  /// 原子关闭标志：memory_order 语义确保 Shutdown 的 happens-before 关系
  std::atomic<bool> is_shutdown_{false};

  /// 组件对应的 Node 实例：用于创建 Reader/Writer，在 Initialize 时创建
  std::shared_ptr<node::Node> node_ = nullptr;

  /// 配置文件路径（从 ComponentConfig::config_file_path 提取）
  std::string config_file_path_;

  /// 类型擦除的 Reader 集合：Shutdown 时统一清理
  std::vector<std::shared_ptr<ReaderBase>> readers_;

  /// Component 协程的唯一调度所有权；关闭时先摘除任务再注销 Reader。
  scheduler::Scheduler* scheduler_ = nullptr;
  uint64_t scheduler_task_id_ = 0;
  std::shared_ptr<data::DataVisitorBase> data_visitor_;
};

}  // namespace component
}  // namespace minicyber

#endif  // MINICYBER_COMPONENT_COMPONENT_BASE_H_
