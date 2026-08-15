#include "minicyber/mainboard/module_controller.h"

#include <dlfcn.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

#include "minicyber/component/component_factory.h"

// =============================================================================
// 日志辅助宏（在 mainboard 独立的日志设施建立前使用 cerr）
// =============================================================================
// mainboard 是独立的可执行文件，不强制依赖 glog/AINFO。
// 这里使用简单的 cerr 做输出，后续可替换为任何日志库。
// =============================================================================
#define MWARN std::cerr << "[Mainboard WARN] "
#define MERROR std::cerr << "[Mainboard ERROR] "
#define MINFO std::cout << "[Mainboard INFO] "

namespace minicyber {
namespace mainboard {

using minicyber::component::ComponentFactory;
using minicyber::component::ComponentBase;
using minicyber::proto::ComponentConfig;
using minicyber::proto::DagConfig;

// =============================================================================
// 构造 / 析构
// =============================================================================

ModuleController::ModuleController(const std::vector<std::string>& dag_paths)
    : dag_paths_(dag_paths) {}

ModuleController::~ModuleController() { Clear(); }

// =============================================================================
// LoadAll：遍历 DAG 路径并逐个加载
// =============================================================================

bool ModuleController::LoadAll() {
  MINFO << "Loading " << dag_paths_.size() << " DAG file(s)..." << std::endl;
  const size_t component_count = component_list_.size();
  const size_t library_count = lib_handles_.size();

  for (const auto& path : dag_paths_) {
    MINFO << "Loading DAG: " << path << std::endl;
    if (!LoadModuleFromFile(path)) {
      MERROR << "Failed to load DAG: " << path << std::endl;
      RollbackTo(component_count, library_count);
      return false;
    }
  }

  MINFO << "All " << dag_paths_.size() << " DAG(s) loaded successfully. "
        << "Total components: " << component_list_.size() << std::endl;
  return true;
}

// =============================================================================
// ParseDagFile：将 .dag 文本 proto 文件解析为 DagConfig
// =============================================================================
// Apollo 的 .dag 配置文件使用 protobuf 的文本格式（TextFormat）；
// 相比二进制格式，文本格式可读性强，适合人工编辑和代码审查。
//
// 实现方式：
//   1. 以文本方式读取整个文件到 std::string
//   2. 使用 TextFormat::ParseFromString 解析为 DagConfig 消息
//
// 错误处理：
//   - 文件不存在或无法打开 → 返回 false，cerr 输出 strerror(errno)
//   - 文件内容不是合法的 TextFormat proto → 返回 false
// =============================================================================

bool ModuleController::ParseDagFile(const std::string& path,
                                    DagConfig* dag_config) {
  // 步骤 1：读取文件到 string
  std::ifstream ifs(path, std::ios::in);
  if (!ifs.is_open()) {
    MERROR << "Cannot open DAG file: " << path << " - "
           << std::strerror(errno) << std::endl;
    return false;
  }

  std::stringstream buf;
  buf << ifs.rdbuf();
  ifs.close();
  const std::string content = buf.str();

  // 步骤 2：解析 TextFormat proto
  // TextFormat 是 protobuf 提供的文本序列化格式解析器。
  // 与二进制格式（ParseFromString）不同，文本格式允许人类直接编辑。
  if (!google::protobuf::TextFormat::ParseFromString(content, dag_config)) {
    MERROR << "Failed to parse DAG file (TextFormat): " << path << std::endl;
    return false;
  }

  MINFO << "Parsed DAG: " << path << " - "
        << dag_config->module_config_size() << " module(s)" << std::endl;
  return true;
}

// =============================================================================
// ResolveLibraryPath：解析 .so 动态库路径
// =============================================================================
// 规则：
//   - 绝对路径（起始字符为 '/'） → 原样返回
//   - 仅库名                      → 保留给 dlopen 的动态库搜索路径
//   - 含目录的相对路径            → 拼接当前工作目录（getcwd）
//
// 为什么不拼接 WorkRoot（Apollo 的做法）：
//   Apollo CyberRT 定义了 WORK_ROOT 环境变量作为基准目录。
//   MiniCyber 目前不定义 WorkRoot，使用 CWD 更通用。
//   CWD 通常是运行 mainboard 的目录。
// =============================================================================

std::string ModuleController::ResolveLibraryPath(
    const std::string& module_library) {
  if (module_library.empty()) {
    return "";
  }
  if (module_library[0] == '/') {
    return module_library;
  }
  // 唯一 DAG 不应写入构建目录；裸 DSO 名称由 MC-616 启动脚本的
  // LD_LIBRARY_PATH 解析。有目录分隔符的历史相对路径继续按 CWD 解析。
  if (module_library.find('/') == std::string::npos) {
    return module_library;
  }
  // 相对路径：拼接 CWD
  char cwd_buf[4096];
  if (::getcwd(cwd_buf, sizeof(cwd_buf)) == nullptr) {
    MWARN << "getcwd failed: " << std::strerror(errno)
          << ", using relative path as-is: " << module_library << std::endl;
    return module_library;
  }
  return std::string(cwd_buf) + "/" + module_library;
}

// =============================================================================
// LoadModuleFromFile：从 .dag 文件路径加载
// =============================================================================

bool ModuleController::LoadModuleFromFile(const std::string& path) {
  DagConfig dag_config;
  if (!ParseDagFile(path, &dag_config)) {
    return false;
  }
  return LoadModule(dag_config);
}

// ---------------------------------------------------------------------------
// LoadModule(const DagConfig&) — 核心加载逻辑
// ---------------------------------------------------------------------------
// 遍历 DagConfig 中的每个 ModuleConfig：
//
//   步骤 1：解析 .so 路径
//     调用 ResolveLibraryPath 获取完整路径。
//     module_library 必须非空，且只能通过 dlopen 加载。
//
//   步骤 2：创建事件驱动组件
//     遍历 ModuleConfig::components：
//       a. ComponentFactory::Create(class_name) → 反射创建
//       b. component->Initialize(config) → 配置+初始化
//       c. component_list_.push_back → 生命周期管理
//
//   步骤 3：错误处理
//     任何一步失败 → 返回 false，由上层 Clear() 统一清理
//
// dlopen 的关键机制（面试核心考点）：
//   dlopen(so_path, RTLD_NOW | RTLD_GLOBAL) 加载 .so 时：
//     1. .so 的全局静态变量构造函数被执行
//     2. 这些构造函数中，MINICYBER_REGISTER_COMPONENT 宏生成的
//        g_registrar_XXX 调用 ComponentFactory::Instance()->Register()
//     3. 由于 RTLD_GLOBAL 共享符号表，.so 中引用的
//        ComponentFactory::Instance() 解析到主进程中的同一单例
//     4. dlopen 返回后，工厂中已注册了 .so 中的 Component 类
//     5. ComponentFactory::Create(class_name) 即可实例化
// ---------------------------------------------------------------------------

bool ModuleController::LoadModule(const DagConfig& dag_config) {
  const size_t component_count = component_list_.size();
  const size_t library_count = lib_handles_.size();
  const auto fail = [this, component_count, library_count]() {
    RollbackTo(component_count, library_count);
    return false;
  };

  for (const auto& module_config : dag_config.module_config()) {
    // ======================================================================
    // 步骤 1：dlopen 加载动态库
    // ======================================================================
    const std::string& lib_path_str = module_config.module_library();
    if (lib_path_str.empty()) {
      MERROR << "module_library is required; static registration is not a "
             << "mainboard loading path." << std::endl;
      return fail();
    }
    std::string load_path = ResolveLibraryPath(lib_path_str);
    MINFO << "Loading library: " << load_path << std::endl;

    // RTLD_NOW 立即暴露 ABI/未解析符号错误；RTLD_GLOBAL 让插件静态注册器
    // 解析到 minicyber_core 中唯一的 ComponentFactory。两者共同构成对齐
    // cyber_ref ModuleController/ClassLoader 的真实 `.so` 边界。
    void* handle = ::dlopen(load_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr) {
      MERROR << "dlopen failed for: " << load_path << std::endl
             << "  dlerror: " << ::dlerror() << std::endl;
      return fail();
    }
    lib_handles_.push_back(handle);

    // ======================================================================
    // 步骤 2：创建事件驱动组件（Component<T>）
    // ======================================================================
    for (const auto& component_info : module_config.components()) {
      if (component_info.class_name().empty() || !component_info.has_config() ||
          component_info.config().name().empty() ||
          component_info.config().readers_size() == 0 ||
          component_info.config().readers(0).channel().empty()) {
        MERROR << "Invalid component DAG entry: class_name, config.name, and "
               << "the first reader channel are required." << std::endl;
        return fail();
      }
      const std::string& class_name = component_info.class_name();
      const ComponentConfig& config = component_info.config();

      // 通过工厂反射创建
      ComponentBase* raw = ComponentFactory::Instance()->Create(class_name);
      if (raw == nullptr) {
        MERROR << "ComponentFactory::Create failed for class: " << class_name
               << std::endl;
        return fail();
      }

      // 包装为 shared_ptr（ComponentBase 继承 enable_shared_from_this）
      bool initialized = false;
      {
        std::shared_ptr<ComponentBase> comp(raw);
        initialized = comp->Initialize(config);
        if (initialized) {
          MINFO << "Component loaded: " << class_name
                << " (node: " << config.name() << ")" << std::endl;
          component_list_.emplace_back(std::move(comp));
        } else {
          // 初始化失败的对象尚未进入列表，也必须在库仍加载时关闭并销毁。
          comp->Shutdown();
        }
      }
      if (!initialized) {
        MERROR << "Component::Initialize failed for: " << class_name
               << " (node: " << config.name() << ")" << std::endl;
        return fail();
      }
    }

  }

  return true;
}

// =============================================================================
// Clear：关闭所有组件并卸载动态库
// =============================================================================
// 执行顺序严格遵守以下依赖关系：
//   1. 先 Shutdown 本水位之后所有组件（停止组件对 Transport/Discovery 的访问）
//   2. 再销毁这些组件（析构代码仍位于已加载 `.so`）
//   3. 最后逆序 dlclose 本水位之后的库
//
// 为什么逆序 Shutdown：
//   如果组件 B 依赖组件 A（B 使用 A 的输出），先 Shutdown A 可能导致
//   B 的 Proc 在 Shutdown 过程中调用时访问已释放的资源。逆序 Shutdown
//   不代替 DAG 拓扑依赖分析；MiniCyber 的裁剪边界只保证本次加载
//   水位内的逆序关闭、销毁与卸载。
// =============================================================================

void ModuleController::Clear() {
  RollbackTo(0, 0);

  MINFO << "ModuleController cleared. Components: 0, Libraries: 0"
        << std::endl;
}

void ModuleController::RollbackTo(size_t component_count, size_t library_count) {
  // 回滚水位只覆盖本次 LoadAll/LoadModule 新增资源，不能误伤调用前已运行
  // 的模块。三个阶段分离，保证任何虚析构均发生在 dlclose 之前。
  for (size_t index = component_list_.size(); index > component_count; --index) {
    if (component_list_[index - 1]) {
      component_list_[index - 1]->Shutdown();
    }
  }
  while (component_list_.size() > component_count) {
    component_list_.pop_back();
  }

  while (lib_handles_.size() > library_count) {
    void* handle = lib_handles_.back();
    lib_handles_.pop_back();
    if (handle != nullptr) {
      ::dlclose(handle);
    }
  }
}

}  // namespace mainboard
}  // namespace minicyber
