#include "minicyber/component/component_factory.h"

namespace minicyber {
namespace component {

// 注册表必须由核心库定义。组件 .so 只引用此符号，dlopen 后的静态注册器
// 因而写入 mainboard 也查询的同一实例，而不是各 DSO 自己的函数局部静态对象。
ComponentFactory* ComponentFactory::Instance() {
  // 插件析构发生在 dlclose 期间。核心注册表故意进程常驻，确保插件注册器
  // 在卸载时仍可安全注销，而不会落入跨 DSO 静态析构顺序。
  static ComponentFactory* instance = new ComponentFactory();
  return instance;
}

}  // namespace component
}  // namespace minicyber
