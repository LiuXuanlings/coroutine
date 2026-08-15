#ifndef MINICYBER_COMPONENT_COMPONENT_FACTORY_H_
#define MINICYBER_COMPONENT_COMPONENT_FACTORY_H_

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "minicyber/component/component_base.h"

// ComponentFactory 是 mainboard 与组件 DSO 之间的唯一注册表。
// MiniCyber 裁剪了 CyberRT 完整 ClassLoader 的多 Loader 管理，但保留
// “dlopen 触发静态注册 -> 按类名创建 -> 卸载前注销”职责。
// Instance 定义在 minicyber_core，保证主进程和 RTLD_GLOBAL DSO
// 解析到同一注册表；本实现不支持运行中替换活跃组件。

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
    return Register(class_name, std::move(creator), nullptr);
  }

  // owner 由 `.so` 内的静态注册器传入。覆盖同名类时，旧库卸载只能撤销
  // 自己的注册，不能删除后加载库的 CreatorFunc。
  bool Register(const std::string& class_name, CreatorFunc creator,
                const void* owner) {
    if (class_name.empty() || !creator) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    registry_[class_name] = Entry{std::move(creator), owner};
    return true;
  }

  /**
   * @brief 移除即将卸载动态库注册的创建函数
   *
   * 对齐 CyberRT ClassLoader 的卸载所有权：`dlclose` 后工厂不能保留指向
   * 已卸载 `.so` 代码段的 CreatorFunc。注册器析构发生在库仍可执行时，先
   * 注销再卸载；这不是面向生产的静态预注册回退路径。
   */
  void Unregister(const std::string& class_name, const void* owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = registry_.find(class_name);
    if (it != registry_.end() && it->second.owner == owner) {
      registry_.erase(it);
    }
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
      creator = it->second.creator;
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

  struct Entry {
    CreatorFunc creator;
    const void* owner = nullptr;
  };

  std::mutex mutex_;
  std::unordered_map<std::string, Entry> registry_;
};

}  // namespace component
}  // namespace minicyber

// 注册宏用匿名命名空间和 __COUNTER__ 保证注册器的内部链接与唯一命名。
// Creator 返回原始指针，ModuleController 立即用 shared_ptr 接管所有权。

#define MINICYBER_REGISTER_COMPONENT_INTERNAL(ClassName, Counter)        \
  namespace {                                                             \
  struct ComponentRegistrar_##ClassName##_##Counter {                     \
    ComponentRegistrar_##ClassName##_##Counter() {                        \
      ::minicyber::component::ComponentFactory::Instance()->Register(     \
          #ClassName,                                                     \
          []() -> ::minicyber::component::ComponentBase* {                \
            return new ClassName();                                       \
          }, this);                                                        \
    }                                                                     \
    ~ComponentRegistrar_##ClassName##_##Counter() {                       \
      ::minicyber::component::ComponentFactory::Instance()->Unregister(   \
          #ClassName, this);                                               \
    }                                                                     \
  };                                                                      \
  static ComponentRegistrar_##ClassName##_##Counter                       \
      g_registrar_##ClassName##_##Counter;                                \
  }

// 使用 __COUNTER__ 保证同一翻译单元多次调用时变量名唯一
#define MINICYBER_REGISTER_COMPONENT(ClassName) \
  MINICYBER_REGISTER_COMPONENT_INTERNAL(ClassName, __COUNTER__)

#endif  // MINICYBER_COMPONENT_COMPONENT_FACTORY_H_
