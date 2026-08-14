#ifndef MINICYBER_COMPONENT_COMPONENT_FACTORY_H_
#define MINICYBER_COMPONENT_COMPONENT_FACTORY_H_

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "minicyber/component/component_base.h"

// =============================================================================
// MiniCyber Component Framework — ComponentFactory 与注册宏
//
// 设计目标：提供通过类名字符串动态实例化 Component 的能力，为 MC-613 的
//   mainboard DAG 解析与 dlopen 动态加载提供唯一注册表。
//
// 与 CyberRT 的差异：
//   CyberRT 使用 Poco 库实现的完整 ClassLoader 体系，包括：
//     - AbstractClassFactoryBase / AbstractClassFactory<Base> / ClassFactory<Derived, Base>
//     - library_path_ 追踪（记录每个类从哪个 .so 加载）
//     - relative_class_loaders_ 管理（支持多 Loader 拥有/释放）
//     - class_loader_utility 全局注册中心
//
//   MiniCyber 只保留单例工厂 + unordered_map<string, CreatorFunc>；Instance
//   定义在 minicyber_core，而不是头文件内联静态变量，保证主进程和以
//   RTLD_GLOBAL 加载的组件 .so 解析到同一注册表。
//
// 静态初始化 + dlopen 机制（面试核心考点）：
//   1. 用户代码在 .cpp 中使用 MINICYBER_REGISTER_COMPONENT(MyComponent)
//   2. 该宏在匿名命名空间中定义一个静态全局变量
//   3. g_registrar_MyComponent 的构造函数在 main() 之前（或 dlopen 加载时）执行
//   4. 构造函数调用 ComponentFactory::Instance()->Register("MyComponent", creator)
//   5. mainboard 调用 ComponentFactory::Instance()->Create("MyComponent")
//      → 返回 new MyComponent()
//
//   当组件被编译为 .so 并通过 dlopen(so_path, RTLD_NOW | RTLD_GLOBAL) 加载时：
//     - .so 内的静态全局变量初始化代码被执行
//     - 因为 RTLD_GLOBAL 共享符号表，.so 中引用的 ComponentFactory::Instance()
//       指向主进程中的同一个单例
//     - 所以 Registration 发生在主进程的工厂中
//     - mainboard 可以像加载本地类一样 Create 来自 .so 的组件
//
//   这就是"动态热加载"的核心原理，不依赖任何反射库，纯 C++ 静态初始化实现。
// =============================================================================

namespace minicyber {
namespace component {

/**
 * @brief 组件工厂单例：管理类名到构造函数映射
 *
 * 典型使用流程：
 *   // 在 .so 的某个 .cpp 中
 *   MINICYBER_REGISTER_COMPONENT(MyComponent)
 *
 *   // 在 mainboard 中
 *   dlopen("libmy_component.so", RTLD_NOW | RTLD_GLOBAL);
 *   auto* comp = ComponentFactory::Instance()->Create("MyComponent");
 *   comp->Initialize(config);
 */
class ComponentFactory {
 public:
  /// 创建者函数签名：返回 ComponentBase*（原始指针，调用方获得所有权）
  using CreatorFunc = std::function<ComponentBase*()>;

  /**
   * @brief 获取单例实例
   *
   * 使用 C++11 线程安全的函数局部静态变量（magic static）。
   * 标准保证：首次 Instance() 调用时创建，且构造过程互斥。
   * 这意味着即使多个 .so 的 dlopen 同时触发静态初始化并调用
   * Instance。
   */
  static ComponentFactory* Instance();

  /**
   * @brief 注册组件类
   *
   * 由 MINICYBER_REGISTER_COMPONENT 宏生成的静态初始化代码调用。
   * 幂等：如果同名类已注册则覆盖（后加载的 .so 优先生效）。
   *
   * @param class_name 组件类名（如 "MyComponent"）
   * @param creator    构造组件的函数对象
   */
  bool Register(const std::string& class_name, CreatorFunc creator) {
    if (class_name.empty() || !creator) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    registry_[class_name] = std::move(creator);
    return true;
  }

  /**
   * @brief 通过类名创建组件实例
   *
   * @param class_name 要创建的组件类名
   * @return ComponentBase* 新创建的组件对象，调用方获得所有权
   * @retval nullptr 类名未注册
   *
   * 调用方负责：
   *   1. 管理返回指针的生命周期（通常包装为 shared_ptr）
   *   2. 调用 Initialize(config) 完成初始化
   *   3. 调用 Shutdown() 完成清理
   *   4. delete 指针释放内存
   */
  ComponentBase* Create(const std::string& class_name) {
    CreatorFunc creator;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = registry_.find(class_name);
      if (it == registry_.end()) {
        return nullptr;
      }
      creator = it->second;
    }
    return creator ? creator() : nullptr;
  }

  /**
   * @brief 检查类名是否已注册
   *
   * @param class_name 组件类名
   * @return true  已注册
   * @return false 未注册
   */
  bool Has(const std::string& class_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return registry_.find(class_name) != registry_.end();
  }

 private:
  ComponentFactory() = default;
  ~ComponentFactory() = default;
  ComponentFactory(const ComponentFactory&) = delete;
  ComponentFactory& operator=(const ComponentFactory&) = delete;

  std::mutex mutex_;
  std::unordered_map<std::string, CreatorFunc> registry_;
};

}  // namespace component
}  // namespace minicyber

// =============================================================================
// MINICYBER_REGISTER_COMPONENT(ClassName) — 组件注册宏
//
// 用法：
//   class MyComponent : public minicyber::component::Component<std::string> {
//     bool Init() override { return true; }
//     bool Proc(const std::shared_ptr<std::string>& msg) override { ... }
//   };
//   MINICYBER_REGISTER_COMPONENT(MyComponent)
//
// 展开效果（假设 ClassName = MyComponent, __COUNTER__ = 42）：
//   namespace {
//     struct ComponentRegistrar_MyComponent_42 {
//       ComponentRegistrar_MyComponent_42() {
//         minicyber::component::ComponentFactory::Instance()->Register(
//             "MyComponent",
//             []() -> minicyber::component::ComponentBase* {
//               return new MyComponent();
//             });
//       }
//     };
//     static ComponentRegistrar_MyComponent_42 g_registrar_MyComponent_42;
//   }
//
// 设计决策说明：
//   1. 匿名命名空间：防止不同翻译单元中相同类名的注册冲突（内部链接）。
//      如果没有匿名命名空间，两个不同的 .so 中各自定义了
//      static ComponentRegistrar_MyComponent g_registrar_MyComponent，
//      链接器可能会合并或产生 ODR 违反。
//
//   2. __COUNTER__：保证同一翻译单元中多个 MINICYBER_REGISTER_COMPONENT
//      调用的变量名不冲突。如果没有 __COUNTER__，在同一 .cpp 中注册
//      两个组件时 g_registrar 的变量名会重复，导致编译错误。
//
//   3. 类名字符串化：#ClassName 在预处理时转为 "ClassName" 字符串。
//
//   4. Creator 返回原始指针：匹配 new 语义，调用方决定所有权模型。
//      如果强制返回 shared_ptr，会迫使所有组件使用 shared_ptr，
//      限制了上层（如 mainboard）的选择。
// =============================================================================

#define MINICYBER_REGISTER_COMPONENT_INTERNAL(ClassName, Counter)        \
  namespace {                                                             \
  struct ComponentRegistrar_##ClassName##_##Counter {                     \
    ComponentRegistrar_##ClassName##_##Counter() {                        \
      ::minicyber::component::ComponentFactory::Instance()->Register(     \
          #ClassName,                                                     \
          []() -> ::minicyber::component::ComponentBase* {                \
            return new ClassName();                                       \
          });                                                             \
    }                                                                     \
  };                                                                      \
  static ComponentRegistrar_##ClassName##_##Counter                       \
      g_registrar_##ClassName##_##Counter;                                \
  }

// 使用 __COUNTER__ 保证同一翻译单元多次调用时变量名唯一
#define MINICYBER_REGISTER_COMPONENT(ClassName) \
  MINICYBER_REGISTER_COMPONENT_INTERNAL(ClassName, __COUNTER__)

#endif  // MINICYBER_COMPONENT_COMPONENT_FACTORY_H_
