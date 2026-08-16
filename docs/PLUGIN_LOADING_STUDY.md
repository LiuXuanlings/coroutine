# 动态插件加载学习笔记

## 一、学习范围

本文只说明 MiniCyber 当前已经实现的动态组件装配链：Protobuf TextFormat 解析 `.dag`、
加载业务 `.so`、组件登记与创建，以及安全关闭和卸载。

当前能力是“运行时按 DAG 动态加载、进程关闭时安全卸载”。它不是运行中的组件版本替换，
因此不应称为热重载。

## 二、先分清四类文件

| 文件 | 作用 | 使用时机 |
|---|---|---|
| `*.proto` | 定义 Protobuf 类型和字段 | 构建时 |
| `*.pb.h`、`*.pb.cc` | `protoc` 从 `.proto` 生成的 C++ 类型代码 | 构建时生成、编译时使用 |
| `*.dag` | 按 Protobuf TextFormat 写出的实际组件配置 | mainboard 运行时读取 |
| `*.so` | 已编译的业务组件共享库 | mainboard 运行后由 `dlopen` 加载 |

`protoc` 和 TextFormat 不能混为一谈：

```text
构建时：dag_conf.proto -> protoc -> dag_conf.pb.h / dag_conf.pb.cc
运行时：autodrive.dag -> TextFormat -> DagConfig 实例
```

`CMakeLists.txt` 用 `add_custom_command` 调用 `protoc`，把生成文件放到
`build/proto_gen/minicyber/proto/`。`-I include` 是 `.proto` 的 `import` 搜索根目录；因此
`import "minicyber/proto/component_conf.proto"` 对应
`include/minicyber/proto/component_conf.proto`。

## 三、Protobuf 配置层次

`include/minicyber/proto/dag_conf.proto` 和
`include/minicyber/proto/component_conf.proto` 定义了唯一 DAG 的主要层次：

```text
DagConfig
  -> 多个 ModuleConfig
       -> 一个 module_library
       -> 多个 ComponentInfo
            -> 一个 class_name
            -> 一个 ComponentConfig
                 -> name、config_file_path、多个 ReaderOption
```

字段含义：

| 类型或字段 | 含义 |
|---|---|
| `message` | 定义一种 Protobuf 类型，不创建对象。 |
| `optional` | 该字段可以不写。是否写过可用 `has_字段名()` 判断。 |
| `repeated` | 该字段可以有零个或多个元素。 |
| `string` | 文本字段，TextFormat 中使用双引号。 |
| `uint32` | 非负整数，例如 `pending_queue_size: 1024`。 |
| `= 1`、`= 2` | 字段固定编号，不是列表下标，也不是字段值。 |

一个 `ComponentInfo` 只有一个 `config`，因为它定义为：

```proto
optional ComponentConfig config = 2;
```

要创建多个组件实例，应写多个 `components { ... }`，而不是在同一个
`ComponentInfo` 中写多个 `config`。

## 四、DAG 文本如何变为 C++ 对象

`ModuleController::ParseDagFile` 先把 `.dag` 文件整体读成临时 `std::string content`，
再调用：

```cpp
google::protobuf::TextFormat::ParseFromString(content, dag_config)
```

调用前，调用方已创建空对象：

```cpp
DagConfig dag_config;
```

并把它的位置传入：

```cpp
ParseDagFile(path, &dag_config);
```

解析成功后，`dag_config` 不保存完整的 DAG 原始文本；它保存按 `.proto` 结构拆开的对象和字段值。
例如 `module_library: "liba.so"` 会成为某个 `ModuleConfig` 实例的 `module_library` 字符串字段。

TextFormat 解析会因字段名错误、值类型不符或花括号不配对而失败。解析失败时不进入动态库加载阶段。

## 五、从 DAG 读取模块和组件

`ModuleController::LoadModule(const DagConfig& dag_config)` 依次读取：

```cpp
for (const auto& module_config : dag_config.module_config()) {
  const std::string& lib_path_str = module_config.module_library();

  for (const auto& component_info : module_config.components()) {
    const std::string& class_name = component_info.class_name();
    const ComponentConfig& config = component_info.config();
  }
}
```

一个对象只能直接读取自己在 `.proto` 中定义的字段：

```text
DagConfig      -> module_config()
ModuleConfig   -> module_library()、components()
ComponentInfo  -> class_name()、config()
ComponentConfig-> name()、readers() 等
```

加载器会拒绝缺少 `module_library`、`class_name`、`config`、实例名或首个 Reader Channel 的条目。
这属于 MiniCyber 的加载规则；它比 Protobuf 的语法规则更严格。

## 六、`.so`、`dlopen` 与路径

`mainboard` 是可直接启动的可执行文件；
`libminicyber_autodrive_components.so` 是不能单独启动的业务共享库。唯一 DAG 中的：

```text
module_library: "libminicyber_autodrive_components.so"
```

告诉 mainboard 本模块的组件代码在哪份共享库中。

`ResolveLibraryPath` 的规则：

| DAG 中的值 | 处理方式 |
|---|---|
| `/opt/demo/libx.so` | 绝对路径，原样使用。 |
| `libx.so` | 纯库名，原样交给 `dlopen` 搜索。 |
| `plugins/libx.so` | 相对路径，拼接当前工作目录。 |

随后加载器调用：

```cpp
void* handle = ::dlopen(load_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
```

`RTLD_NOW` 要求在加载时立即检查组件库需要的外部符号；找不到时 `dlopen` 失败。
`RTLD_GLOBAL` 使本次动态加载的公共符号可供之后加载的库使用。MiniCyber 的 mainboard、核心库和
组件库依赖同一个 `ComponentFactory` 符号边界。

加载成功时 `handle` 非空，并保存到 `lib_handles_`；失败时 `handle == nullptr`，可通过
`dlerror()` 取得错误文本。

## 七、工厂登记而不是 `dlsym`

`dlsym` 是另一种可行的插件接口。它按导出的普通函数名查找函数，例如：

```cpp
extern "C" ComponentBase* CreatePerceptionComponent() {
  return new PerceptionComponent();
}
```

`extern "C"` 让该函数保留稳定、明确的导出名称；直接依赖 C++ 构造函数名称并不合适，
因为构造函数不是普通的“无参数、返回新对象地址”的创建函数，且 C++ 名称可能被编译器改写。

MiniCyber 当前不使用 `dlsym`。它使用 `ComponentFactory`，即“类名到创建方法”的共同登记表：

```text
"PerceptionComponent"
  -> 以后执行 new PerceptionComponent() 的创建方法
```

组件源码末尾的：

```cpp
MINICYBER_REGISTER_COMPONENT(PerceptionComponent)
```

会生成随 `.so` 存在的静态登记员。`dlopen` 加载 `.so` 时，登记员构造并调用 `Register`，
把类名和创建方法存入核心库中的共同工厂表。此时尚未创建业务组件对象。

mainboard 后续调用：

```cpp
ComponentFactory::Instance()->Create(class_name)
```

才会根据 DAG 的 `class_name` 查表、执行创建方法并实际 `new` 出组件对象。

组件源码注册的字符串与 DAG 的 `class_name` 必须一致：前者用于登记，后者用于查询。
核心库必须持有唯一工厂表；若插件登记到私有表而 mainboard 查询另一张表，`Create` 将找不到组件。

## 八、组件实例配置

`class_name` 和 DAG 外层 `config.name` 的职责不同：

| 字段 | 含义 |
|---|---|
| `class_name` | 选择哪一种组件代码，例如 `PerceptionComponent`。 |
| 外层 `config.name` | 当前运行实例的名称，用于创建该实例的 Node。 |

同一个组件类可以有多个实例，应写多个 `components` 条目。例如前、后摄像头都可使用
`PerceptionComponent`，但实例名分别为 `front_perception` 和 `rear_perception`。

当前 DAG 外层 `config` 中的 `config_file_path` 指向组件私有配置文件。组件随后再次用
TextFormat 解析该私有文件，得到另一个独立的 `ComponentConfig` 对象。

当前 `PerceptionComponent` 的私有文件只含：

```text
name: "perception"
```

其 `Init()` 检查私有配置中的 `name` 是否为固定字符串 `perception`，用于发现私有配置文件
拿错的情形。当前实现不比较外层实例名和私有文件中的 `name`；因此私有配置名不是实例名。

## 九、创建、回滚和卸载

创建成功后，mainboard 立即用：

```cpp
std::shared_ptr<ComponentBase> comp(raw);
```

接管组件对象。初始化成功后，`component_list_` 持有它；初始化失败时，先调用 `Shutdown()`，
再让 `shared_ptr` 销毁对象。

`Shutdown()` 的职责是停止组件任务、Reader、Writer 和 Node 等运行资源；它必须在完整组件对象
仍然存在时执行，不能依赖默认析构函数代替。

每次加载开始前，`LoadModule` 记录组件和库句柄数量作为水位。某次加载失败时，
`RollbackTo` 只回滚本次水位之后新建的资源，不能清空此前已成功加载的模块。

关闭与回滚的严格顺序：

```text
逆序 Shutdown 新组件
  -> 逆序销毁组件对象
  -> 逆序调用 dlclose(handle)
  -> dlclose 执行过程中静态登记员析构并 Unregister
  -> .so 卸载完成
```

不能先 `dlclose` 再销毁组件，因为组件析构函数和虚函数代码位于 `.so` 中；库已卸载后再调用这些
代码会造成无效跳转。

## 十、真实 DAG 的一次加载

`config/autodrive/autodrive.dag` 当前包含一份组件库和五个组件条目：

```text
libminicyber_autodrive_components.so
  -> PerceptionComponent
  -> FusionComponent
  -> PlanningComponent
  -> ControlComponent
  -> ControlAuditComponent
```

因此正常启动时：

```text
dlopen：1 次
lib_handles_：1 个句柄
component_list_：5 个成功初始化的组件对象
```

关闭时按组件列表逆序关闭和销毁，最后才对唯一组件库调用 `dlclose`。

## 十一、源码复习顺序

建议按以下顺序阅读，每一步只验证一个问题：

1. `include/minicyber/proto/dag_conf.proto`：DAG 有哪些类型和字段？
2. `include/minicyber/proto/component_conf.proto`：组件运行配置有哪些字段？
3. `config/autodrive/autodrive.dag`：这些字段写成了什么实际值？
4. `CMakeLists.txt`：`.proto` 如何生成 C++ 类型？业务组件如何成为 `.so`？
5. `src/mainboard/module_controller.cpp`：如何解析、加载、创建、回滚和卸载？
6. `include/minicyber/component/component_factory.h`：类名如何登记为创建方法？
7. `demo/autodrive/components/perception_component.cpp`：具体业务组件如何注册和读取私有配置？

完成上述阅读后，再进入 Node、Reader、Writer、DataVisitor 与 Scheduler 链路；这些属于组件成功
初始化之后的数据处理阶段，不应和 Protobuf 配置或动态加载边界混读。
