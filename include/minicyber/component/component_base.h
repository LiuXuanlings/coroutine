#ifndef MINICYBER_COMPONENT_COMPONENT_BASE_H_
#define MINICYBER_COMPONENT_COMPONENT_BASE_H_

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "minicyber/proto/component_conf.pb.h"
#include "minicyber/node/node.h"

// =============================================================================
// MiniCyber Component Framework — ComponentBase 生命周期抽象
//
// 设计目标：对齐 Apollo CyberRT 的 ComponentBase，为业务组件提供规范化的
//   Initialize / Shutdown 生命周期。业务开发者只需继承 Component<T> 并重写
//   Init() 和 Proc(), 框架自动管理 Node 创建、Reader 注册和资源释放。
//
// 与 CyberRT 的差异（Step 29）：
//   1. 去掉了 gflags 依赖（flag_file_path 不处理 gflags 加载）
//   2. 去掉了 scheduler::RemoveTask（Phase 7 再加入）
//   3. 去掉了 class_loader / common::WorkRoot / common::GetAbsolutePath
//      （这些是 CyberRT 的 ClassLoader 和文件工具，MiniCyber 暂不需要）
//   4. GetProtoConfig 为 Stub 实现（需要 text_format 解析时再开启）
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
using minicyber::proto::TimerComponentConfig;

/**
 * @brief 组件生命周期基类（抽象接口）
 *
 * 继承层次：
 *   ComponentBase                    ← 生命周期、Node 引用、配置路径
 *     ├── Component<M0>              ← 单通道事件驱动组件
 *     ├── Component<M0, M1>          ← 双通道融合组件（Phase 7）
 *     └── TimerComponent             ← 定时触发组件
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
  // Component<M0>::Initialize() 或 TimerComponent::Initialize() 重写。
  //
  // 两个重载分别对应事件驱动和定时驱动两种组件模式。
  // 这是 C++ 重载（overload）而非覆盖（override），因为参数类型不同。
  // ==========================================================================

  virtual bool Initialize(const ComponentConfig& config) { (void)config; return false; }
  virtual bool Initialize(const TimerComponentConfig& config) { (void)config; return false; }

  /**
   * @brief 组件关闭（幂等）
   *
   * 关闭顺序严格遵守：
   *   1. is_shutdown_.exchange(true) — 原子检查 + 设置，防止重入
   *   2. Clear() — 派生类自定义清理（虚函数派发）
   *   3. 遍历 readers_ 逐个 Shutdown — 停止数据接收
   *   4. readers_.clear() — 释放 Reader 资源
   *
   * 不在此处调用 scheduler::RemoveTask() 的原因：
   *   MiniCyber 当前的 Scheduler 没有 RemoveTask 接口（因为任务是和 Processor
   *   生命周期绑定的）。当 Shutdown 被调用后，Processor 循环会在下一次迭代
   *   发现协程已 FINISHED 后自动清理。这个设计差异在 Phase 7 引入 Choreography
   *   调度时会重新审视。
   */
  virtual void Shutdown() {
    if (is_shutdown_.exchange(true, std::memory_order_acq_rel)) {
      return;  // 第二次调用直接跳过
    }
    Clear();

    for (auto& reader : readers_) {
      reader->Shutdown();
    }
    readers_.clear();
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
   * [Stub 实现] — 当前直接返回 false。
   *
   * 完整实现需要使用 google::protobuf::TextFormat::ParseFromString()
   * 配合 std::ifstream 读取 config_file_path_ 指向的文本 proto 文件。
   * 由于 MiniCyber 当前阶段还没有真实组件需要加载配置文件，这里先留空。
   *
   * 未来启用方式（需要包含 <fstream> 和 <google/protobuf/text_format.h>）：
   *   std::ifstream ifs(config_file_path_);
   *   if (!ifs.is_open()) return false;
   *   std::string content(std::istreambuf_iterator<char>(ifs), {});
   *   return google::protobuf::TextFormat::ParseFromString(content, config);
   */
  template <typename T>
  bool GetProtoConfig(T* config) const {
    (void)config;
    return false;
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
  //   注意：Clear 调用时 readers_ 还未被销毁，派生类依然可以访问它们。
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

  void LoadConfigFiles(const TimerComponentConfig& config) {
    if (!config.config_file_path().empty()) {
      config_file_path_ = config.config_file_path();
    }
  }

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
};

}  // namespace component
}  // namespace minicyber

#endif  // MINICYBER_COMPONENT_COMPONENT_BASE_H_
