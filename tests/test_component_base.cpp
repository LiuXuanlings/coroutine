#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <unistd.h>

#include "minicyber/component/component_base.h"
#include "minicyber/proto/component_conf.pb.h"

// =============================================================================
// DummyComponent — 用于测试 ComponentBase 生命周期的辅助类
//
// 设计要点：
//   - 继承 ComponentBase，重写 Init/Clear 以便追踪调用
//   - 重写 Initialize 使 Init() 被调用（基类默认返回 false 不调 Init）
//   - 暴露 init_called_ / clear_called_ 供测试断言
// =============================================================================
class DummyComponent : public minicyber::component::ComponentBase {
 public:
  bool init_called() const { return init_called_; }
  bool clear_called() const { return clear_called_; }

  // 允许测试重置内部状态标记（验证 Shutdown 幂等性时需要）
  void ResetFlags() {
    init_called_ = false;
    clear_called_ = false;
  }

  // 重写 Initialize: 创建 Node 并调用 Init()
  // 真正的 Component<T> 还会创建 Reader，此处简化为仅测试生命周期
  bool Initialize(const minicyber::proto::ComponentConfig& config) override {
    node_.reset(new minicyber::node::Node(config.name()));
    LoadConfigFiles(config);
    if (!Init()) return false;
    return true;
  }

 protected:
  bool Init() override {
    init_called_ = true;
    return true;
  }

  void Clear() override {
    clear_called_ = true;
  }

 private:
  bool init_called_ = false;
  bool clear_called_ = false;
};

// =============================================================================
// MinimalComponent — 不重写 Initialize，测试基类默认行为
// =============================================================================
class MinimalComponent : public minicyber::component::ComponentBase {
 protected:
  bool Init() override { return true; }
};

// =============================================================================
// 测试用例
// =============================================================================

// ---------------------------------------------------------------------------
// 基类 Initialize 默认返回 false
// ---------------------------------------------------------------------------
// 验证 ComponentBase::Initialize(const ComponentConfig&) 的默认实现返回 false。
// 这确保没有重写 Initialize 的组件不会被意外启动。
// ---------------------------------------------------------------------------
TEST(ComponentBaseTest, BaseInitializeReturnsFalse) {
  MinimalComponent comp;
  minicyber::proto::ComponentConfig config;
  config.set_name("minimal");
  EXPECT_FALSE(comp.Initialize(config));
}

// ---------------------------------------------------------------------------
// Shutdown 完整生命周期：Init → Clear → is_shutdown
// ---------------------------------------------------------------------------
// 验证 Shutdown 的正确执行顺序：
//   1. Init() 在 Initialize 时被调用 → init_called == true
//   2. Shutdown() 设置关闭标志 → IsShutdown() == true
//   3. Shutdown() 调用 Clear() → clear_called == true
//   4. readers_ 为空 → reader Shutdown 循环安全执行（零迭代）
// ---------------------------------------------------------------------------
TEST(ComponentBaseTest, ShutdownLifecycle) {
  auto comp = std::make_shared<DummyComponent>();
  minicyber::proto::ComponentConfig config;
  config.set_name("lifecycle_test");
  ASSERT_TRUE(comp->Initialize(config));
  EXPECT_TRUE(comp->init_called());
  EXPECT_FALSE(comp->IsShutdown());

  comp->Shutdown();
  EXPECT_TRUE(comp->IsShutdown());
  EXPECT_TRUE(comp->clear_called());
}

// ---------------------------------------------------------------------------
// Shutdown 幂等性：第二次调用无效果
// ---------------------------------------------------------------------------
// exchange(true) 保证：只有第一次成功设置 is_shutdown_ 的线程能进入清理逻辑，
// 后续调用直接 return。C++ 原子操作的 exchange 语义保证了即使多线程同时
// 调用 Shutdown，也只有一个线程执行清理。
// ---------------------------------------------------------------------------
TEST(ComponentBaseTest, ShutdownIsIdempotent) {
  auto comp = std::make_shared<DummyComponent>();
  minicyber::proto::ComponentConfig config;
  config.set_name("idempotent_test");
  comp->Initialize(config);

  comp->Shutdown();
  EXPECT_TRUE(comp->IsShutdown());
  EXPECT_TRUE(comp->clear_called());

  // 重置标记，验证第二次 Shutdown 不会再次调用 Clear
  comp->ResetFlags();
  comp->Shutdown();
  EXPECT_FALSE(comp->clear_called());  // Clear 不应再次被调用
}

// ---------------------------------------------------------------------------
// GetProtoConfig 按 TextFormat 加载组件配置
// ---------------------------------------------------------------------------
// 空路径返回 false；真实文件必须解析出原始 Protobuf 字段。
// ---------------------------------------------------------------------------
TEST(ComponentBaseTest, GetProtoConfigLoadsTextProto) {
  DummyComponent comp;
  minicyber::proto::ComponentConfig empty;
  EXPECT_FALSE(comp.GetProtoConfig(&empty));

  char path[] = "/tmp/minicyber_component_config_XXXXXX";
  const int fd = ::mkstemp(path);
  ASSERT_NE(fd, -1);
  ::close(fd);
  {
    std::ofstream output(path);
    output << "name: \"loaded_component\"\n";
  }

  minicyber::proto::ComponentConfig init_config;
  init_config.set_name("loader");
  init_config.set_config_file_path(path);
  ASSERT_TRUE(comp.Initialize(init_config));
  minicyber::proto::ComponentConfig config;
  ASSERT_TRUE(comp.GetProtoConfig(&config));
  EXPECT_EQ(config.name(), "loaded_component");
  ::unlink(path);
}

// ---------------------------------------------------------------------------
// 直接 Shutdown（未 Initialize）应安全
// ---------------------------------------------------------------------------
// 组件可能在没有调用 Initialize 的情况下被销毁，此时 Shutdown 应该
// 安全执行：node_ 为 nullptr，readers_ 为空，Clear() 空实现。
// ---------------------------------------------------------------------------
TEST(ComponentBaseTest, ShutdownWithoutInitialize) {
  auto comp = std::make_shared<DummyComponent>();
  EXPECT_FALSE(comp->init_called());
  EXPECT_FALSE(comp->IsShutdown());

  // 未 Initialize 直接 Shutdown → 安全无 crash
  comp->Shutdown();
  EXPECT_TRUE(comp->IsShutdown());
}
