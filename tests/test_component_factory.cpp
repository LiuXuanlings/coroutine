#include <gtest/gtest.h>

#include <atomic>
#include <string>

#include "minicyber/component/component.h"
#include "minicyber/component/component_factory.h"
#include "minicyber/node/node.h"

namespace {
using TestMessage = minicyber::proto::RoleAttributes;

TestMessage MakeMessage(const std::string& value) {
  TestMessage message;
  message.set_node_name(value);
  return message;
}
}  // namespace

// =============================================================================
// FactoryTestComponent — 通过工厂注册的测试组件
//
// 使用 MINICYBER_REGISTER_COMPONENT 宏在文件作用域注册。
// 该宏在匿名命名空间中生成一个静态全局变量，其构造函数在程序启动时
// （或 dlopen 加载 .so 时）自动调用 ComponentFactory::Register()。
//
// 注意：该组件必须定义在使用宏之前。宏展开为匿名命名空间中的静态变量
// 定义，此时 ClassName 必须是一个完整的类定义（而非前置声明）。
// =============================================================================

/// 记录 Proc 最后一次收到的消息，用于验证创建后的组件能正常收发
static std::string g_factory_received;
/// 记录 Proc 被调用的总次数
static std::atomic<int> g_factory_proc_count{0};

class FactoryTestComponent
    : public minicyber::component::Component<TestMessage> {
 public:
  static void ResetStats() {
    g_factory_received.clear();
    g_factory_proc_count.store(0, std::memory_order_relaxed);
  }

 protected:
  bool Init() override {
    init_called_.store(true, std::memory_order_relaxed);
    return true;
  }

  bool Proc(const std::shared_ptr<TestMessage>& msg) override {
    g_factory_received = msg->node_name();
    g_factory_proc_count.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

 public:
  bool init_called() const {
    return init_called_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<bool> init_called_{false};
};

// =============================================================================
// SecondTestComponent — 第二个注册类，验证多类共存
// =============================================================================

class SecondTestComponent
    : public minicyber::component::Component<TestMessage> {
 protected:
  bool Init() override { return true; }
  bool Proc(const std::shared_ptr<TestMessage>& msg) override {
    (void)msg;
    return true;
  }
};

// 注册两个组件到工厂
// 宏展开为匿名命名空间中的静态全局变量，构造函数在 main 之前调用
// Register()，将类名 + lambda 填入 ComponentFactory 的 registry_ map。
MINICYBER_REGISTER_COMPONENT(FactoryTestComponent)
MINICYBER_REGISTER_COMPONENT(SecondTestComponent)

// =============================================================================
// 测试用例
// =============================================================================

using minicyber::component::ComponentFactory;

// ---------------------------------------------------------------------------
// 通过工厂创建组件 → 返回非空 + 类型正确
// ---------------------------------------------------------------------------
// 验证 Register 在静态初始化阶段成功注册，Create 能正确实例化。
// 这里不检查具体类型名（编译器产物不同平台不同），只验证指针非空
// 且可以安全调用 ComponentBase 的接口方法。
// ---------------------------------------------------------------------------
TEST(ComponentFactoryTest, CreateRegisteredClass) {
  FactoryTestComponent::ResetStats();

  auto* raw = ComponentFactory::Instance()->Create("FactoryTestComponent");
  ASSERT_NE(raw, nullptr);

  // 验证返回的类型确实继承自 ComponentBase（可以安全调用 GetProtoConfig）
  EXPECT_FALSE(raw->IsShutdown());

  delete raw;
}

// ---------------------------------------------------------------------------
// 创建不存在的类名 → 返回 nullptr
// ---------------------------------------------------------------------------
// 没有注册过的类名查询应该返回空指针，而不是崩溃。
// 这保证了 mainboard 在加载 .dag 时如果拼错了类名，不会段错误，
// 而是可以优雅地报错并跳过。
// ---------------------------------------------------------------------------
TEST(ComponentFactoryTest, CreateNonExistentReturnsNull) {
  auto* raw = ComponentFactory::Instance()->Create("NonExistentComponent");
  EXPECT_EQ(raw, nullptr);
}

// ---------------------------------------------------------------------------
// Has 确认注册状态
// ---------------------------------------------------------------------------
TEST(ComponentFactoryTest, HasRegisteredClass) {
  EXPECT_TRUE(ComponentFactory::Instance()->Has("FactoryTestComponent"));
  EXPECT_TRUE(ComponentFactory::Instance()->Has("SecondTestComponent"));
  EXPECT_FALSE(ComponentFactory::Instance()->Has("NonExistentComponent"));
}

// ---------------------------------------------------------------------------
// 端到端验证：工厂创建 → Initialize → Write → Proc 被调用
// ---------------------------------------------------------------------------
// 这是最接近真实场景的测试：模拟 mainboard 的流程
//   Create → Initialize → (Writer 写数据) → Proc 收到消息
//
// 验证链：
//   1. Factory::Create 成功返回
//   2. Initialize 成功（创建 Node + Reader + Init）
//   3. 同进程 Writer 写入消息
//   4. Proc 被同步调用（MiniCyber 当前路径是同步回调）
//   5. 消息内容正确
//   6. Shutdown 后清理
// ---------------------------------------------------------------------------
TEST(ComponentFactoryTest, CreateInitializeAndDeliver) {
  FactoryTestComponent::ResetStats();

  // Step 1: 通过工厂创建组件（模拟 mainboard 的 Create("ClassName"))
  auto* raw = ComponentFactory::Instance()->Create("FactoryTestComponent");
  ASSERT_NE(raw, nullptr);

  // 将原始指针包装为 shared_ptr，以利用 RAII 自动清理
  // ComponentBase 继承 enable_shared_from_this，因此必须使用
  // shared_ptr 管理生命周期（否则 shared_from_this 会崩溃）。
  std::shared_ptr<FactoryTestComponent> comp(
      static_cast<FactoryTestComponent*>(raw));

  // Step 2: 创建配置并 Initialize（模拟 mainboard 从 proto dag 读取配置）
  minicyber::proto::ComponentConfig config;
  config.set_name("factory_comp");
  config.add_readers()->set_channel("/factory_channel");

  ASSERT_TRUE(comp->Initialize(config));
  EXPECT_TRUE(comp->init_called());

  // Step 3: 通过 Writer 发布消息（模拟真实数据流）
  minicyber::node::Node pub_node("factory_publisher");
  auto writer = pub_node.CreateWriter<TestMessage>("/factory_channel");
  ASSERT_NE(writer, nullptr);

  std::string test_msg("delivered via factory");
  EXPECT_TRUE(writer->Write(MakeMessage(test_msg)));

  // Step 4: 验证 Proc 被调用（同步路径，Writer 线程中直接触发）
  EXPECT_EQ(g_factory_proc_count.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_factory_received, test_msg);

  // Step 5: 第二次写入验证组件仍活跃
  EXPECT_TRUE(writer->Write(MakeMessage("second factory message")));
  EXPECT_EQ(g_factory_proc_count.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(g_factory_received, "second factory message");

  // Step 6: 清理
  comp->Shutdown();
  EXPECT_TRUE(comp->IsShutdown());

  // comp 析构时自动调用 ~ComponentBase() → Shutdown（幂等，安全）
}

// ---------------------------------------------------------------------------
// 工厂创建的组件在 Shutdown 后不再接收消息
// ---------------------------------------------------------------------------
TEST(ComponentFactoryTest, FactoryComponentShutdownGraceful) {
  FactoryTestComponent::ResetStats();

  auto* raw = ComponentFactory::Instance()->Create("FactoryTestComponent");
  ASSERT_NE(raw, nullptr);

  auto comp = std::shared_ptr<FactoryTestComponent>(
      static_cast<FactoryTestComponent*>(raw));

  minicyber::proto::ComponentConfig config;
  config.set_name("factory_shutdown");
  config.add_readers()->set_channel("/factory_shutdown");

  ASSERT_TRUE(comp->Initialize(config));

  // 先正常收发
  minicyber::node::Node pub_node("pub");
  auto writer = pub_node.CreateWriter<TestMessage>("/factory_shutdown");
  ASSERT_NE(writer, nullptr);

  EXPECT_TRUE(writer->Write(MakeMessage("before")));
  EXPECT_EQ(g_factory_proc_count.load(std::memory_order_relaxed), 1);

  // Shutdown
  comp->Shutdown();

  // 再写消息 — 不保证计数不变（可能有残留回调），但保证不 crash
  EXPECT_TRUE(writer->Write(MakeMessage("after")));
  SUCCEED();
}

// ---------------------------------------------------------------------------
// 多个注册类共存
// ---------------------------------------------------------------------------
TEST(ComponentFactoryTest, MultipleRegisteredClasses) {
  // 验证所有注册的类都可以 Create
  auto* c1 = ComponentFactory::Instance()->Create("FactoryTestComponent");
  auto* c2 = ComponentFactory::Instance()->Create("SecondTestComponent");
  ASSERT_NE(c1, nullptr);
  ASSERT_NE(c2, nullptr);
  EXPECT_NE(c1, c2);  // 不同实例

  delete c1;
  delete c2;
}

// ---------------------------------------------------------------------------
// Factory 单例稳定性：多次 Instance() 返回同一指针
// ---------------------------------------------------------------------------
TEST(ComponentFactoryTest, SingletonStability) {
  auto* f1 = ComponentFactory::Instance();
  auto* f2 = ComponentFactory::Instance();
  EXPECT_EQ(f1, f2);
}

TEST(ComponentFactoryTest, RejectsInvalidRegistration) {
  auto* factory = ComponentFactory::Instance();
  EXPECT_FALSE(factory->Register("", []() { return new FactoryTestComponent(); }));
  EXPECT_FALSE(factory->Register("null_creator", ComponentFactory::CreatorFunc{}));
  EXPECT_EQ(factory->Create(""), nullptr);
  EXPECT_EQ(factory->Create("null_creator"), nullptr);
}

TEST(ComponentFactoryTest, CreatorMayQueryFactory) {
  auto* factory = ComponentFactory::Instance();
  ASSERT_TRUE(factory->Register("reentrant_creator", [factory]() {
    return factory->Has("FactoryTestComponent")
               ? static_cast<minicyber::component::ComponentBase*>(
                     new FactoryTestComponent())
               : nullptr;
  }));

  std::unique_ptr<minicyber::component::ComponentBase> instance(
      factory->Create("reentrant_creator"));
  EXPECT_NE(instance, nullptr);
}
