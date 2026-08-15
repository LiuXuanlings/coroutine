#ifndef MINICYBER_MAINBOARD_MODULE_CONTROLLER_H_
#define MINICYBER_MAINBOARD_MODULE_CONTROLLER_H_

#include <memory>
#include <string>
#include <vector>

#include "minicyber/component/component_base.h"
#include "minicyber/proto/dag_conf.pb.h"

// =============================================================================
// MiniCyber Mainboard — ModuleController
//
// 设计目标：对齐 Apollo CyberRT 的 ModuleController / mainboard，作为框架的
//   标准化启动入口。读取 DAG 配置文件，通过 dlopen 动态加载业务组件 .so，
//   利用 ComponentFactory 反射创建组件实例，构建运行时计算图。
//
// 启动流程（mainboard 主流程）：
//   1. main() 解析唯一 DAG 路径和独立 Scheduler 配置
//   2. ModuleController 构造 → 存储 DAG 路径
//   3. LoadAll() → 遍历 DAG 路径，逐个解析并加载
//      a. ParseDagFile(path) → protobuf TextFormat 解析为 DagConfig
//      b. LoadModule(DagConfig) → 遍历 ModuleConfig 列表
//         i.   dlopen(module_library) → 加载 .so，触发静态注册
//         ii.  ComponentFactory::Create(class_name) → 反射创建
//         iii. component->Initialize(config) → 初始化组件
//      c. 存储到 component_list_ 进行生命周期管理
//   4. WaitForShutdown() → 阻塞等待 SIGINT/SIGTERM
//   5. Clear() → Shutdown、销毁所有组件后才 dlclose 所有 .so 句柄
//
// 与 CyberRT 的差异：
//   - 去掉 ModuleArgument 类（参数解析在 main() 中完成）
//   - 去掉 ClassLoaderManager（使用 ComponentFactory 替代）
//   - 去掉 common::GetProtoFromFile / WorkRoot（直接 fstream + TextFormat）
//   - 去掉 GlobalData / component_nums 统计（组件数直接从 factory 获取）
//
// 路径解析规则：
//   - 绝对路径（以 '/' 开头）：直接使用
//   - 相对路径（其他）：拼接当前工作目录（CWD）为前缀
// =============================================================================

namespace minicyber {
namespace mainboard {

using minicyber::component::ComponentBase;
using minicyber::proto::DagConfig;

class ModuleController {
 public:
  /**
   * @brief 构造控制器
   * @param dag_paths DAG 配置文件路径列表
   */
  explicit ModuleController(const std::vector<std::string>& dag_paths);

  ~ModuleController();

  /**
   * @brief 加载所有 DAG 配置，启动计算图
   *
   * 遍历 dag_paths_，对每个路径：
   *   1. 解析 proto 文本文件为 DagConfig
   *   2. 调用 LoadModule(DagConfig) 完成加载
   *
   * @return true  全部加载成功
   * @return false 任一 DAG 加载失败（失败后已卸载已加载的组件）
   */
  bool LoadAll();

  /**
   * @brief 从内存中的 DagConfig 加载模块（可被 LoadAll 和单元测试调用）
   *
   * 遍历 dag_config.module_config() 列表：
   *   1. 解析 module_library 路径 → dlopen
   *   2. 遍历 components → ComponentFactory::Create → Initialize
   *
   * @param dag_config 已解析的计算图配置
   * @return true  全部加载成功
   * @return false 任一模块加载失败
   */
  bool LoadModule(const DagConfig& dag_config);

  /**
   * @brief 关闭所有组件并卸载动态库
   *
   * 执行顺序：
   *   1. 遍历 component_list_ 逐个 Shutdown（逆序释放）
   *   2. 清空 component_list_
   *   3. 遍历 lib_handles_ 逐个 dlclose
   *   4. 清空 lib_handles_
   *
   * 插件静态注册器在 dlclose 前析构并按所有权注销创建函数；业务插件不得
   * 产生 GNU unique 单例，否则 glibc 会延迟析构而使该不变量失效。
   */
  void Clear();

  /**
   * @brief 获取已加载的组件数量（用于测试/调试）
   */
  size_t ComponentCount() const { return component_list_.size(); }

  /**
   * @brief 获取已加载的动态库数量（用于测试/调试）
   */
  size_t LibraryCount() const { return lib_handles_.size(); }

 private:
  /**
   * @brief 从文件路径加载 DAG 配置
   *
   * 内部调用 ParseDagFile 将 .dag 文本文件解析为 DagConfig proto，
   * 再委托给 LoadModule(DagConfig)。
   *
   * @param path .dag 文件路径
   * @return true 加载成功
   * @return false 解析或加载失败
   */
  bool LoadModuleFromFile(const std::string& path);

  // 回滚一次 LoadAll/LoadModule 尝试中新创建的组件和动态库。
  void RollbackTo(size_t component_count, size_t library_count);

  /**
   * @brief 解析 .dag 文本格式的配置文件
   *
   * Apollo 的 .dag 文件是 protobuf 的文本格式（TextFormat），
   * 而非二进制格式。使用 google::protobuf::TextFormat::Parse
   * 从 std::ifstream 读取并填充 DagConfig 消息。
   *
   * @param path      .dag 文件路径
   * @param dag_config [out] 解析结果
   * @return true  解析成功
   * @return false 文件不存在或格式错误
   */
  bool ParseDagFile(const std::string& path, DagConfig* dag_config);

  /**
   * @brief 解析动态库路径
   *
   *   - 绝对路径（以 '/' 开头）：直接返回
   *   - 相对路径：拼接 CWD
   *
   * @param module_library proto 中的 module_library 字段
   * @return std::string 解析后的完整路径
   */
  static std::string ResolveLibraryPath(const std::string& module_library);

  /// mainboard 的唯一 DAG 配置路径；vector 保留是为了复用 LoadAll 接口测试。
  std::vector<std::string> dag_paths_;

  /// 已加载的组件列表（管理生命周期）
  std::vector<std::shared_ptr<ComponentBase>> component_list_;

  /// 已加载的动态库句柄列表；所有关联组件销毁后才允许 dlclose。
  std::vector<void*> lib_handles_;
};

}  // namespace mainboard
}  // namespace minicyber

#endif  // MINICYBER_MAINBOARD_MODULE_CONTROLLER_H_
