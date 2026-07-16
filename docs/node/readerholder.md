### 一、ReaderBaseHolder 多态持有异构 Reader，GetReader<T> 按类型取回
这是 C++ 中典型的**类型擦除 + 多态容器**设计，用来解决「不同模板类型的 Reader 无法存入同一容器」的问题。

#### 1. 为什么需要这个设计？
`Reader<T>` 是一个类模板，当消息类型 `T` 不同时（比如 `Reader<std::string>` 和 `Reader<int>`），它们是完全独立、没有继承关系的不同类型。
如果想让 `Node` 统一管理所有类型的 Reader，并存入同一个 `std::map`，直接存储是做不到的——STL 容器要求元素类型必须一致。

#### 2. 多态持有：用基类擦除类型
- 定义无模板的基类 `ReaderBaseHolder`，只提供虚析构函数，作为统一的「外壳类型」。它不依赖模板参数 `T`，所有派生类都可以用它的智能指针表示。
- 定义派生类模板 `ReaderHolder<T>`，继承自 `ReaderBaseHolder`，内部真正持有具体的 `std::shared_ptr<Reader<T>>`。每一种消息类型 `T` 都会实例化出对应的派生类。

在 `CreateReader` 中，创建完具体的 `Reader<T>` 后，会把它包装进 `ReaderHolder<T>`，再**向上转型**为基类 `std::shared_ptr<ReaderBaseHolder>`，存入 `readers_` 这个 map：
```cpp
auto holder = std::make_shared<ReaderHolder<T>>(r);
readers_[channel] = holder; // 基类指针存入map，具体类型被"擦除"
```

这样一来，无论消息类型是什么，都可以放进同一个 `map<std::string, shared_ptr<ReaderBaseHolder>>` 里统一管理，这就是**多态持有异构 Reader**。

#### 3. 按类型取回：向下转型恢复类型
在 `GetReader<T>` 中，通过 `std::dynamic_pointer_cast` 把基类指针**向下转型**回具体的 `ReaderHolder<T>`：
```cpp
auto holder = std::dynamic_pointer_cast<ReaderHolder<T>>(it->second);
if (holder == nullptr) return nullptr;
return holder->reader;
```
- 如果实际存储的 Reader 类型和你传入的 `T` 一致，转型成功，就能拿到类型安全的 `Reader<T>` 指针；
- 如果类型不匹配，转型失败返回 `nullptr`，保证类型安全。

这就是**按类型取回**。

---

### 二、创建时自动向 TopologyManager 注册 writer/reader 角色
`TopologyManager`（拓扑管理器）是维护全局通信拓扑的模块：记录「哪个节点 → 在哪个 channel 上 → 是读还是写」的映射关系，用于服务发现、链路可视化、依赖管理等。

#### 1. 节点自身的自动注册
`Node` 构造函数中，创建节点时就自动向拓扑管理器注册节点信息：
```cpp
explicit Node(const std::string& name) : node_name_(name) {
  topology::TopologyManager::Instance()->AddNode(node_name_, ::getpid());
}
```
用户只要创建 `Node` 对象，就自动完成节点注册，无需手动调用。

#### 2. Writer/Reader 角色的自动注册
代码注释明确说明：**拓扑注册在 Writer/Reader::Init 时完成**。
在 `CreateWriter` 和 `CreateReader` 中，对象创建后会立刻调用 `Init()` 方法：
```cpp
// CreateWriter 中
auto w = std::make_shared<Writer<T>>(node_name_, channel);
w->Init(); // Init 内部自动完成 writer 角色的拓扑注册

// CreateReader 中
auto r = std::make_shared<Reader<T>>(node_name_, channel, callback);
r->Init(); // Init 内部自动完成 reader 角色的拓扑注册
```

也就是说：
- 用户调用 `CreateWriter` / `CreateReader` 创建对象时，内部自动执行了注册逻辑；
- 用户不需要手动调用拓扑注册接口，只要创建了读写者，就自动把「该节点在该 channel 上的读/写角色」上报给了 TopologyManager。

这种设计把拓扑注册的细节封装在内部，对用户透明，保证了拓扑数据的完整性。


---

### 一、按需选型
`Reader<T>` 和 `Writer<T>` 都是类模板，`T` 不同就是完全无关的类型，不能直接塞进同一个 `std::vector` / `std::map`（STL 容器要求元素类型统一）。

所以两边都必须做**类型擦除**：用一个“不带模板参数的通用类型”当外壳，把具体的 `Reader<T>`/`Writer<T>` 包起来，再放进容器。

---

### 二、为什么 Reader 必须用 Holder 多态那套
Reader 有一个硬性需求：**支持 `GetReader<T>` 按类型把对象取回来**。

```cpp
// 用户需要能这样用：先创建，后面还能按 channel + 类型取出来
auto r = node.CreateReader<std::string>("/chatter", cb);
// ...
auto r2 = node.GetReader<std::string>("/chatter");
```

要“存进去还能按类型安全取回来”，就必须保留**类型标识**，C++ 里最标准的做法就是**多态 + 向下转型**：
1.  定义无模板的基类 `ReaderBaseHolder`（带虚析构，有虚表）；
2.  派生模板类 `ReaderHolder<T>` 包裹具体 `Reader<T>`；
3.  存的时候向上转型成基类指针，类型被“擦除”；
4.  取的时候用 `std::dynamic_pointer_cast` 向下转型，靠虚表判断类型对不对，对就成功，不对返回 `nullptr`，保证类型安全。

**这是唯一能做到「安全按类型取回」的轻量方案**，没有更简单的写法。代价就是要多写一个基类 + 一个派生模板类，结构稍重。

---

### 三、为什么 Writer 敢用 `shared_ptr<void>` 
反观 Writer，它**没有 `GetWriter` 接口**。

用户调用 `CreateWriter` 拿到 `shared_ptr<Writer<T>>` 之后，就自己拿着用了；Node 对 Writer 只有一个诉求：
> **只要 Node 活着，Writer 就不能提前析构；Node 死的时候，Writer 跟着一起释放。**

说白了就是：**只托管生命周期，不需要再取出来**。

既然不需要“取回”，那就没必要保留类型信息，也不用多态、不用虚表，用最轻量的方式就行——这就是 `shared_ptr<void>` + 自定义删除器的由来。

#### 这个写法的原理（C++ 标准特性，不是黑魔法）
`std::shared_ptr` 有一个关键特性：**删除器和对象的析构逻辑，是存在控制块里的，和指针类型本身无关**。哪怕你把它转型成 `shared_ptr<void>`，销毁时依然会调用最初绑定的删除器，正确析构原对象。

```cpp
writers_.push_back(std::shared_ptr<void>(
    static_cast<void*>(nullptr), 
    [w](void*) { (void)w; }  // 按值捕获 w，相当于持有一份引用计数
));
```
- 这个 `shared_ptr<void>` 本身指向空，真正的作用是**通过 lambda 按值捕获 `w`，把 Writer 的引用计数 +1**；
- 只要这个 `shared_ptr<void>` 还在 `writers_` 容器里，Writer 对象就不会被释放；
- Node 析构时清空 `writers_`，这个 `shared_ptr<void>` 销毁 → lambda 销毁 → 捕获的 `w` 被释放 → Writer 析构。

它就是一个**纯生命周期锚点**，不提供任何访问能力，只负责“保活”。代码极短，完全满足需求，没有多余成本。

---

### 四、一句话总结为什么不统一写法
| 方案 | 能力 | 适用场景 |
| :--- | :--- | :--- |
| ReaderBaseHolder 多态 | 既能托管生命周期，又能按类型安全取回 | Reader：需要 GetReader 取回使用 |
| shared_ptr<void> + 删除器 | 只能托管生命周期，无法取回 | Writer：只需要保活，不需要从 Node 取回 |

不是作者故意搞两种写法折腾人，是**需求决定实现**：
- Reader 有取回需求，必须上稍重的多态方案；
- Writer 只有保活需求，就用最极简的生命周期锚点。

如果未来要给 Node 加 `GetWriter<T>` 接口，那 Writer 也得改成 Holder 那套结构，不然没法安全转型取回。