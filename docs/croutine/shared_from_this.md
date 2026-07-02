# 深度拆解 std::enable_shared_from_this / shared_from_this
## 前置基础
`std::shared_ptr` 分为两部分：
1. **托管对象**：你自己写的类实例（`Foo`）
2. **控制块（Control Block）**：堆上单独分配，存储引用计数、弱计数、析构函数、内存销毁逻辑
**核心规则**：同一个对象只能绑定**唯一一个控制块**；只要新建 `shared_ptr<T>(裸指针)`，就会分配全新独立控制块。

---

# 一、核心问题深度拆解：为什么 `shared_ptr(this)` 会两套计数、双重释放
## 1. 错误代码复现
```cpp
#include <memory>
#include <iostream>
struct A {
    std::shared_ptr<A> getSelf() {
        return std::shared_ptr<A>(this); // 致命错误
    }
};

int main() {
    auto p1 = std::make_shared<A>(); // p1：对象A + 控制块#1
    auto p2 = p1->getSelf();         // p2：用裸指针this新建shared_ptr，分配控制块#2
    
    std::cout << p1.use_count() << std::endl; // 输出1（仅控制块#1计数）
    std::cout << p2.use_count() << std::endl; // 输出1（仅控制块#2计数）
    // 函数结束，p2先析构：控制块#2计数归0，delete this 销毁A
    // 再析构p1：控制块#1计数归0，再次delete已经销毁的A → double free 崩溃
}
```
## 2. 为什么会产生两套独立控制块？
`this` 仅仅是一个**无附加信息的裸指针**，它只存对象内存地址，**不携带任何控制块信息**。
当执行 `shared_ptr<A>(this)` 时，标准库逻辑：
1. 看不到这个 `this` 已经被别的 `shared_ptr` 托管；
2. 标准库默认逻辑：传入裸指针 = 需要新建一套独立控制块；
3. 于是直接在堆分配全新控制块，和外部 `p1` 的控制块完全隔离、互不感知；
两个智能指针各管一套计数，互不叠加，销毁时各自释放对象。

## 3. std::enable_shared_from_this 底层原理：weak_ptr 藏在哪、如何绑定、weak转shared完整流程
### 3.1 weak_ptr 藏在哪里？
`enable_shared_from_this<T>` 是**模板基类**，内部私有成员：
```cpp
template<class T>
class enable_shared_from_this {
private:
    weak_ptr<T> _weak_this; // 私有弱指针，藏在这里
public:
    shared_ptr<T> shared_from_this();
    weak_ptr<T> weak_from_this() noexcept;
};
```
你写 `class Foo : public enable_shared_from_this<Foo>` 时，`Foo` 对象内存里会继承自带一个 `weak_ptr<T>` 成员，这就是存储控制块关联信息的载体。

### 3.2 make_shared 如何自动绑定这个 weak_ptr？
`std::make_shared<T>(args...)` 做两件事：
1. **一次性分配一块连续内存**：同时存放 `T` 对象 + 配套控制块；
2. **T 构造完成后做检测**：如果 `T` 继承了 `enable_shared_from_this<T>`，自动执行：
   ```cpp
   this->_weak_this = shared_ptr<T>(托管对象地址, 本次创建的控制块地址);
   ```
   直接把当前唯一控制块绑定到基类内置的 `_weak_this`。

补充：如果用 `shared_ptr<T>(new T)` 裸new构造，逻辑完全一致，同样会自动填充基类里的 `weak_ptr`。

### 3.3 shared_from_this()：weak_ptr 转 shared_ptr 的完整过程
调用 `shared_from_this()` 时执行逻辑：
1. 取出基类私有成员 `_weak_this`（已经绑定唯一控制块）；
2. 调用 `weak_ptr::lock()` 方法：
   - 检查控制块强引用计数是否大于0（对象存活）；
   - 如果存活：**复用原有控制块**，强计数+1，返回新的 `shared_ptr<T>`；
   - 如果对象已销毁：抛出异常 `std::bad_weak_ptr`；
3. 最终生成的shared_ptr和外部原有智能指针共享同一个控制块，计数互通，不会重复释放。

### 对比总结
- `shared_ptr(this)`：裸指针新建独立控制块，计数隔离；
- `shared_from_this()`：从内置weak_ptr复用已有控制块，计数统一。

---

# 二、四大使用场景
## 场景1：异步回调（asio网络、多线程任务）
### 业务背景
网络编程最经典场景：异步读写是非阻塞的，发起读操作后函数立刻返回；数据到达后系统自动执行lambda回调。
回调执行时，必须保证当前会话对象没有被提前销毁，否则访问成员变量会野指针崩溃。

```cpp
#include <memory>
#include <iostream>

// 模拟socket
struct Socket{};

struct Session : std::enable_shared_from_this<Session> {
    Socket sock;

    // 对外接口：启动一次异步读取
    void startRecv() {
        // 捕获self，延长对象生命周期
        auto self = shared_from_this();
        // 模拟异步读：操作系统等待数据，数据就绪后执行回调lambda
        asyncRead(sock, [self](int dataLen) {
            // 回调触发：收到数据，走数据处理逻辑
            self->handleRecvData(dataLen);
        });
    }

    // 数据处理函数：收到数据后执行业务
    void handleRecvData(int len) {
        std::cout << "收到数据长度：" << len << std::endl;
    }
};

// 模拟异步接口
void asyncRead(Socket&, auto callback) {
    // 伪代码：底层网络库，等待数据，就绪后异步执行callback
}

int main() {
    auto sess = std::make_shared<Session>();
    sess->startRecv();
    return 0;
}
```
### 关键点解释
1. `self = shared_from_this()` 捕获到lambda里：只要回调还未执行，self持有shared_ptr，对象不会析构；
2. 如果不捕获self：外部`sess`析构后，对象销毁，后续回调触发时访问成员直接崩溃。

## 场景2：类内部返回自身shared_ptr（工厂模式、注册逻辑）
### 两个名词解释
1. **工厂模式**：不允许外部直接`new`/`make_shared`创建对象，类内部提供静态函数统一生成实例，对外返回自身shared_ptr，统一管理生命周期；
2. **注册逻辑**：对象创建后，需要把自己注册到全局管理器，管理器需要持有shared_ptr维持对象存活。

### 完整可运行示例
```cpp
#include <memory>
#include <vector>
#include <string>
#include <iostream>

// 全局对象管理器
struct NodeManager {
    std::vector<std::shared_ptr<class Node>> nodeList;
    void registerNode(std::shared_ptr<class Node> n) {
        nodeList.push_back(n);
    }
};
NodeManager g_nodeMgr;

class Node : public std::enable_shared_from_this<Node> {
private:
    std::string name;
    // 私有构造：禁止外部new，标准工厂模式特征
    Node(std::string n) : name(n) {}
public:
    // 工厂静态函数：唯一创建入口
    static std::shared_ptr<Node> create(std::string name) {
        auto newNode = std::make_shared<Node>(name);
        // 注册逻辑：创建完成立刻把自己注册进全局管理器
        newNode->registerSelf();
        return newNode;
    }

    // 注册自身到管理器
    void registerSelf() {
        // 不能传this裸指针，必须用shared_from_this()生成共享智能指针
        g_nodeMgr.registerNode(shared_from_this());
    }

    std::shared_ptr<Node> getSelf() {
        return shared_from_this();
    }
};

int main() {
    // 只能通过工厂create创建，无法直接new Node("test")
    auto n1 = Node::create("节点1");
    auto n2 = n1->getSelf(); // n1、n2共用控制块 use_count=2
    return 0;
}
```

## 场景3：观察者/订阅模式
### 标准观察者模式角色定义
1. **观察者 Observer**：监听事件、收到通知后执行业务；
2. **被观察者（主题 Subject）**：产生事件，维护所有订阅自己的观察者列表；
3. **管理器 Manager**：全局中介，存储所有主题、所有观察者，统一分发事件。

### 代码角色：
逻辑：Observer（观察者）需要把自己注册给Subject（被观察者），Subject容器存储`shared_ptr<Observer>`，保证通知时观察者不销毁。
```cpp
#include <memory>
#include <vector>
#include <iostream>

// 观察者：监听事件，必须enable_shared_from_this才能注册
struct Observer : std::enable_shared_from_this<Observer> {
    std::string name;
    Observer(std::string n) : name(n) {}
    // 收到通知回调
    void onNotify(int msg) {
        std::cout << name << " 收到消息：" << msg << std::endl;
    }
};

// 被观察者（主题）：产生事件，保存所有订阅的观察者
struct Subject {
    std::vector<std::shared_ptr<Observer>> watchers;
    // 订阅：观察者把自己注册到主题
    void subscribe(std::shared_ptr<Observer> obs) {
        watchers.push_back(obs);
    }
    // 广播事件，通知所有观察者
    void notifyAll(int msg) {
        for (auto& w : watchers) w->onNotify(msg);
    }
};

// 全局管理器：管理所有事件主题
struct EventManager {
    std::vector<Subject> subjects;
    Subject& getMainSubject() { return subjects[0]; }
};
EventManager g_eventMgr;

int main() {
    // 创建观察者（必须make_shared）
    auto obs1 = std::make_shared<Observer>("观察者A");
    auto obs2 = std::make_shared<Observer>("观察者B");

    Subject& mainSub = g_eventMgr.getMainSubject();
    // 注册：调用shared_from_this传递shared_ptr
    mainSub.subscribe(obs1->shared_from_this());
    mainSub.subscribe(obs2->shared_from_this());

    mainSub.notifyAll(1001); // 广播消息，两个观察者接收
    return 0;
}
```
### 角色总结
1. Observer（观察者）：代码里继承enable_shared_from_this的类，需要注册自己；
2. Subject（被观察者/主题）：产生消息、保存观察者列表；
3. EventManager（管理器）：全局容器，统一管理所有主题；
### 为什么这里必须用shared_from_this？
订阅时要把自身存入Subject的vector，vector存shared_ptr，如果直接传裸指针this，会出现双重控制块，提前析构崩溃。

## 场景4：树形父子结构（关联场景2，区分互补关系）
### 和场景2的关联与区别
- 相同点：都需要在成员函数内部生成自身shared_ptr；
- 不同点：树形结构存在**双向引用**（父存子shared_ptr，子不能存父shared_ptr，否则循环引用内存泄漏），需要搭配weak_ptr；场景2无双向引用，单纯注册。

### 完整示例
```cpp
#include <memory>
#include <vector>

struct TreeNode : std::enable_shared_from_this<TreeNode> {
    std::vector<std::shared_ptr<TreeNode>> children;
    // 子节点存储父节点：weak_ptr，避免循环引用泄漏
    std::weak_ptr<TreeNode> parent;

    // 添加子节点
    void addChild(std::shared_ptr<TreeNode> child) {
        // 当前节点作为父，赋值给子节点的parent弱指针
        child->parent = weak_from_this();
        children.push_back(child);
    }
};

int main() {
    auto root = std::make_shared<TreeNode>();
    auto child = std::make_shared<TreeNode>();
    root->addChild(child);

    // 从子节点获取父节点
    if (auto p = child->parent.lock()) {
        // p是root的shared_ptr
    }
    return 0;
}
```
### 核心重点
1. 父节点通过`shared_from_this()`把自身转换成weak_ptr赋值给子节点；
2. 如果子节点用`shared_ptr<TreeNode> parent`存储父：root持有child，child持有root，循环引用，两者永远不会析构，内存泄漏；
3. weak_ptr不增加引用计数，完美解决树形双向关联问题。

---

# 三、禁止调用 shared_from_this() 的场景重申（带底层原因）
1. **栈对象**：栈分配没有shared_ptr控制块，基类`_weak_this`为空，lock()抛异常；
2. **裸new未托管**：`TreeNode* p = new TreeNode;` 对象没有绑定任何控制块，weak_ptr空；
3. **构造函数内调用**：对象构造阶段，`make_shared`还未执行weak_ptr绑定逻辑，`_weak_this`未初始化；
4. 对象所有shared_ptr全部析构销毁后调用：控制块强计数为0，lock失败抛异常。

# 四、极简总结
1. 裸指针`this`无控制块信息，直接构造shared_ptr会新建独立控制块，双重释放崩溃；
2. `enable_shared_from_this`内置私有`weak_ptr`，make_shared创建对象时自动绑定唯一控制块；
3. `shared_from_this()`通过内置weak_ptr的lock()复用已有控制块，保证计数统一；
4. 使用场景统一本质：类由shared_ptr管理，成员函数需要向外传递指向自身的共享智能指针（回调、注册、树形结构、工厂）；
5. 循环引用场景搭配`weak_from_this()`获取弱指针，解决内存泄漏。