# MiniCyber ComponentFactory 涉及C++技术知识点文档（精简版）
## 文档概述
本文档基于 `component_factory.h` 源码，梳理组件静态自注册框架依赖的核心语言特性、链接与插件化相关技术。

## 5. std::function 类型擦除
```cpp
using CreatorFunc = std::function<ComponentBase*()>;
```
### 知识点
1. `std::function<返回类型(参数)>` 提供**类型擦除**能力；
2. 可以统一容纳普通函数、函数对象、Lambda；
3. 不同组件的构造Lambda类型各不相同，依靠`std::function`包装后存入同一个哈希表；
4. 是实现“统一保存任意组件构造器”的关键基础设施。

## 7. C++预处理宏体系
### 7.1 `#` 字符串化运算符
`#ClassName`：预处理阶段将标识符直接转换成字符串字面量。
示例：`#MyComponent` → `"MyComponent"`，避免手写字符串带来的名字不一致问题。

### 7.2 `##` 标记粘贴运算符
`ComponentRegistrar_##ClassName##_##Counter`
预处理阶段拼接标识符，动态生成结构体名称、静态变量名称。

### 7.3 `__COUNTER__` 编译器内置宏
GCC/Clang/MSVC均支持；每一次展开自动自增；
作用：同一`.cpp`文件多次调用注册宏时，生成唯一标识，防止变量/类型重定义编译报错。

### 7.4 宏分层封装
对外暴露简洁宏`MINICYBER_REGISTER_COMPONENT`，内部宏接收类名+计数器参数，隔离复杂度。

## 8. 匿名命名空间 `namespace { ... }`
```cpp
namespace {
// 注册器结构体 + static 静态变量
}
```
### 知识点
匿名命名空间内所有实体具备**内部链接（internal linkage）**：
1. 符号仅在当前翻译单元（.cpp）内可见；
2. 多个动态库`.so`中存在同名注册器变量时，规避ODR冲突；
3. 和静态变量配合双重隔离名字，是插件开发常用手段。

## 9. 静态存储期变量自动初始化（整套自注册框架核心）
```cpp
static ComponentRegistrar_MyComponent_0 g_registrar_MyComponent_0;
```
### 知识点
1. 该变量属于**静态存储期**；
2. 两种执行时机：
   - 静态链接进主程序：`main()`执行**之前**调用构造函数；
   - 编译为动态库`.so`：`dlopen(RTLD_NOW)`加载库时执行构造函数；
3. 在注册器构造函数内自动执行工厂`Register`，实现**零手动调用自动注册**；
4. 业界通称：**C++静态自注册模式**，无反射实现插件化的经典方案。

## 12. 对象所有权：裸指针传递
```cpp
ComponentBase* Create(const std::string& class_name);
```
### 知识点
1. 接口返回裸指针，对象所有权转移给调用方；
2. 调用方需要手动`delete`释放内存，或者包装为`std::shared_ptr`管理生命周期；
3. 框架设计取舍：不强制使用智能指针，向上层调度框架开放生命周期选择；
4. 风险：使用不当容易引发内存泄漏。

## 13. 动态库 dlopen 配套技术（跨SO插件化基础）
加载示例：
```cpp
dlopen("libxxx.so", RTLD_NOW | RTLD_GLOBAL);
```
### 知识点
1. `RTLD_NOW`：立即解析所有符号，触发.so内部静态变量构造函数，完成组件注册；
2. `RTLD_GLOBAL`：动态库符号并入全局符号表；
3. 关键效果：.so内部调用`ComponentFactory::Instance()`与主程序访问的是**同一个单例**；
4. 限制：当前实现不记录so句柄，不支持`dlclose`动态卸载；卸载后注册表残留构造函数，再次创建实例会触发未定义行为（段错误）。

## 14. ODR（One Definition Rule）单定义规则规避
### 知识点
C++ ODR规则：同一个实体不能在多个翻译单元多次定义，否则产生未定义行为。
本框架双层手段规避冲突：
1. 匿名命名空间：注册器类型、静态变量拥有内部链接；
2. `__COUNTER__` 生成唯一标识符；
保证不同.cpp、不同.so中的注册器名称互不冲突，避免链接异常。
