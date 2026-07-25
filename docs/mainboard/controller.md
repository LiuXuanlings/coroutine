# MiniCyber ModuleController — 知识点及代码详解文档

> 基于 `module_controller.h` / `module_controller.cc`，对齐 Apollo CyberRT 的简化实现。

---

## 一、整体架构定位

`ModuleController` 是 **MiniCyber Mainboard** 的核心类，职责可概括为：

| 职责 | 说明 |
|------|------|
| **配置解析** | 读取 `.dag` 文本文件，通过 protobuf `TextFormat` 反序列化为 `DagConfig` |
| **动态加载** | 通过 `dlopen` 加载业务 `.so`，触发静态注册，将组件类注入 `ComponentFactory` |
| **反射创建** | 通过工厂模式 + 字符串类名，运行时创建组件实例 |
| **生命周期管理** | 维护 `component_list_`（组件）和 `lib_handles_`（动态库句柄），支持统一初始化与清理 |

---

## 二、类设计精要

### 2.1 成员变量职责分离

```cpp
std::vector<std::string> dag_paths_;                          // 输入：配置源
std::vector<std::shared_ptr<ComponentBase>> component_list_;  // 运行时：组件对象
std::vector<void*> lib_handles_;                              // 运行时：.so 句柄
```

**设计意图**：
- `dag_paths_` 仅保存路径，不持有文件句柄，符合 **RAII** 最小持有原则。
- `component_list_` 使用 `shared_ptr` 管理组件生命周期，`ComponentBase` 继承 `enable_shared_from_this`，支持在组件内部安全获取 `shared_ptr`。
- `lib_handles_` 保存 `dlopen` 返回的 `void*`，确保 `Clear()` 时能精确 `dlclose`。

### 2.2 析构函数即清理

```cpp
~ModuleController() { Clear(); }
```

**知识点**：利用析构函数保证异常安全。无论 `LoadAll()` 中途返回 `false` 还是正常退出栈对象，`Clear()` 都会被调用，避免资源泄漏。

---

## 三、启动流程详解（LoadAll）

```
main() 传入 dag_paths
    │
    ▼
ModuleController::LoadAll()
    │
    ├── 遍历 dag_paths_
    │       │
    │       ▼
    │   LoadModuleFromFile(path)
    │       │
    │       ├── ParseDagFile(path, &dag_config)
    │       │       ├── ifstream 读取全文到 string
    │       │       └── TextFormat::ParseFromString(content, dag_config)
    │       │
    │       └── LoadModule(dag_config)
    │               │
    │               ├── 遍历 module_config
    │               │       │
    │               │       ├── Step 1: dlopen(module_library)
    │               │       │       └── lib_handles_.push_back(handle)
    │               │       │
    │               │       ├── Step 2: 遍历 components
    │               │       │       ├── ComponentFactory::Create(class_name)
    │               │       │       ├── comp->Initialize(config)
    │               │       │       └── component_list_.push_back(comp)
    │               │       │
    │               │       └── Step 3: 遍历 timer_components（同上）
    │               │
    │               └── 任一失败 → return false
    │
    └── 任一 DAG 失败 → MERROR + return false
```

---

## 四、核心知识点逐段拆解

### 4.1 protobuf TextFormat 解析

```cpp
std::stringstream buf;
buf << ifs.rdbuf();
const std::string content = buf.str();

google::protobuf::TextFormat::ParseFromString(content, dag_config);
```

**知识点**：
- Apollo 的 `.dag` 文件采用 **protobuf 文本格式**（非二进制），便于人工编辑和代码审查。
- 实现上先通过 `rdbuf()` 将文件流完整读入 `std::string`，再调用 `ParseFromString`。
- 为什么不直接用 `ZeroCopyInputStream`？因为 `ZeroCopyInputStream` 需要配合 `FileInputStream` 使用，而这里为了简化依赖、统一错误处理，选择了最通用的 `string` 中转方式。对于 DAG 文件（通常 < 10KB），内存拷贝开销可忽略。

### 4.2 动态库路径解析

```cpp
static std::string ResolveLibraryPath(const std::string& module_library) {
  if (module_library[0] == '/') return module_library;
  char cwd_buf[4096];
  ::getcwd(cwd_buf, sizeof(cwd_buf));
  return std::string(cwd_buf) + "/" + module_library;
}
```

**知识点**：
- **绝对路径**（`/` 开头）直接使用，**相对路径**拼接当前工作目录（CWD）。
- 与 Apollo CyberRT 的差异：Apollo 使用 `WORK_ROOT` 环境变量作为基准目录，MiniCyber 简化此设计，使启动行为更直观（在哪运行 mainboard，就以哪为根）。
- `getcwd` 失败时的降级策略：返回原相对路径，让 `dlopen` 自行解析（可能依赖 `LD_LIBRARY_PATH`）。

### 4.3 dlopen + 静态注册（面试核心）

```cpp
void* handle = ::dlopen(load_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
lib_handles_.push_back(handle);

// dlopen 后，ComponentFactory 已包含 .so 中注册的类
ComponentBase* raw = ComponentFactory::Instance()->Create(class_name);
```

**知识点：RTLD_NOW vs RTLD_LAZY**
- `RTLD_NOW`：立即解析所有未定义符号。优点是运行时不会出现延迟加载导致的符号解析错误。
- `RTLD_GLOBAL`：将 `.so` 中的符号导出到全局符号表。

**知识点：静态注册机制**
- 业务 `.so` 中通常包含类似如下的宏：
  ```cpp
  static auto g_registrar = ComponentFactory::Instance()->Register(
      "MyComponent", []() -> ComponentBase* { return new MyComponent(); });
  ```
- 该变量是 **全局静态对象**，在 `dlopen` 加载 `.so` 时，其构造函数被执行，从而将类名和创建 lambda 注册到 `ComponentFactory` 的单例中。
- 由于 `RTLD_GLOBAL`，`.so` 中引用的 `ComponentFactory::Instance()` 解析到主进程中的同一单例，实现了跨动态库的单例共享。

### 4.4 错误处理与回滚

```cpp
bool ModuleController::LoadAll() {
  for (const auto& path : dag_paths_) {
    if (!LoadModuleFromFile(path)) {
      MERROR << "Failed to load DAG: " << path << std::endl;
      return false;   // 注意：这里直接返回，但析构函数会调用 Clear()
    }
  }
  return true;
}
```

**知识点**：
- **失败即回滚**：`LoadAll` 在任一 DAG 失败时返回 `false`。由于 `ModuleController` 是栈对象或成员对象，其析构函数调用 `Clear()`，会自动：
  1. 逆序 `Shutdown` 已创建的组件；
  2. `dlclose` 已加载的 `.so`。
- 这种设计避免了"加载一半留下脏状态"的问题，符合 **强异常安全（Strong Exception Safety）** 的基本保证。

### 4.5 组件生命周期管理

```cpp
// 创建
ComponentBase* raw = ComponentFactory::Instance()->Create(class_name);
std::shared_ptr<ComponentBase> comp(raw);
comp->Initialize(config);
component_list_.emplace_back(std::move(comp));

// 销毁
for (auto it = component_list_.rbegin(); it != component_list_.rend(); ++it) {
  if (*it) (*it)->Shutdown();
}
component_list_.clear();
```

**知识点**：
- **两步初始化**：`Create` 仅分配对象，`Initialize` 完成配置绑定和资源申请。这种分离允许工厂模式统一创建，而初始化逻辑由具体组件定制。
- **逆序 Shutdown**：先启动的组件可能被子组件依赖，逆序关闭可减少资源竞争和空指针访问风险。虽然完整的依赖管理需要 DAG 拓扑排序（注释中提到"留待 Phase 7"），但逆序是一个实用的工程折中。


---

## 五、与 Apollo CyberRT 的差异对比

| 特性 | Apollo CyberRT | MiniCyber（本实现） |
|------|----------------|---------------------|
| 参数解析 | `ModuleArgument` 类 | 直接在 `main()` 中完成 |
| 类加载管理 | `ClassLoaderManager` | `ComponentFactory` 单例 |
| proto 文件读取 | `common::GetProtoFromFile` | 直接 `ifstream` + `TextFormat` |
| 工作目录 | `WORK_ROOT` 环境变量 | 当前工作目录 CWD |
| 全局统计 | `GlobalData` / `component_nums` | 直接从 `component_list_.size()` 获取 |
| 依赖关闭 | 无特殊处理 | 逆序 `Shutdown`（拓扑排序待实现） |

---

## 六、面试高频考点总结

1. **为什么用 `RTLD_GLOBAL`？**  
   使 `.so` 符号全局可见，后续加载的其他 `.so` 可以引用，同时保证 `ComponentFactory` 单例地址一致。

2. **`dlopen` 后为什么能直接 `Create`？**  
   全局静态对象的构造函数在 `dlopen` 时执行，已完成注册。

3. **`ParseDagFile` 为什么先读 `string` 再解析？**  
   `TextFormat::ParseFromString` 需要完整文本；对于小文件，一次读取到内存简单可靠。

4. **失败时如何回滚？**  
   返回 `false` 后，栈析构触发 `Clear()`，自动 `Shutdown` + `dlclose`。

5. **为什么 `Shutdown` 要逆序？**  
   减少依赖组件被提前释放导致的悬垂访问风险（工程折中，非完美方案）。
