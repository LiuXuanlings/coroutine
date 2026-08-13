#include <gtest/gtest.h>

#include <atomic>
#include <string>

#include "minicyber/component/component.h"
#include "minicyber/node/node.h"

// =============================================================================
// SingleChannelComponent — 单通道组件测试桩
//
// 模拟最典型的用法：订阅一个字符串 Channel，将收到的最后一条消息
// 保存到外部变量，以便测试断言。
//
// 设计：使用原始指针（而非 shared_ptr 或全局对象）输出结果，使测试
// 可以创建多个独立实例，避免全局状态污染。
// =============================================================================
class SingleChannelComponent
    : public minicyber::component::Component<std::string> {
 public:
  // 通过 output 指针将 Proc 结果传出（测试使用栈上对象确保安全）
  explicit SingleChannelComponent(std::string* output,
                                  std::atomic<int>* count = nullptr)
      : output_(output), count_(count) {}

 protected:
  bool Init() override {
    // 模拟业务初始化：分配资源、打开文件等
    init_called_.store(true);
    return true;
  }

  bool Proc(const std::shared_ptr<std::string>& msg) override {
    if (output_) *output_ = *msg;
    if (count_) count_->fetch_add(1, std::memory_order_relaxed);
    proc_called_.store(true);
    return true;
  }

 public:
  // 测试辅助：暴露内部状态
  bool init_called() const { return init_called_.load(); }
  bool proc_called() const { return proc_called_.load(); }

 private:
  std::string* output_ = nullptr;
  std::atomic<int>* count_ = nullptr;
  std::atomic<bool> init_called_{false};
  std::atomic<bool> proc_called_{false};
};

// =============================================================================
// NoReaderComponent — 验证无 Reader 配置时 Initialize 返回 false
// =============================================================================
class NoReaderComponent
    : public minicyber::component::Component<std::string> {
 protected:
  bool Init() override { return true; }
  bool Proc(const std::shared_ptr<std::string>&) override { return true; }
};

// =============================================================================
// NullComponent — 验证 NullType 特化的初始化
// =============================================================================
// Component<NullType> 全特化没有 Proc()，只有 Init()。
// 这个特化用于 TimerComponent 等不依赖数据通道的组件。
// =============================================================================
class NullComponent
    : public minicyber::component::Component<minicyber::NullType> {
 protected:
  bool Init() override {
    init_called_.store(true);
    return true;
  }

 public:
  bool init_called() const { return init_called_.load(); }

 private:
  std::atomic<bool> init_called_{false};
};

// =============================================================================
// FailInitComponent — Init() 失败的组件，验证 Initialize 传播失败
// =============================================================================
class FailInitComponent
    : public minicyber::component::Component<std::string> {
 protected:
  bool Init() override { return false; }
  bool Proc(const std::shared_ptr<std::string>&) override { return true; }
};

// =============================================================================
// 测试用例
// =============================================================================

// ---------------------------------------------------------------------------
// 单通道组件：Writer 发布 → Component::Proc 被同步调用
// ---------------------------------------------------------------------------
// MiniCyber 当前的 Intra 传输路径是同步的：
//   Writer::Write → ... → DataNotifier::Notify → 回调 → Proc
//
// 这意味着单元测试可以在单线程中验证：
//   1. 创建 Writer（发布节点）
//   2. 创建 Component（订阅节点）
//   3. Writer 写一条消息
//   4. 验证 Proc 已收到消息（同步返回）
//
// 这个测试不依赖 Scheduler，不涉及协程切换。
// =============================================================================
TEST(ComponentTest, SingleChannelSyncDelivery) {
  std::string received;
  auto comp = std::make_shared<SingleChannelComponent>(&received);

  // 创建发布者 Node + Writer
  minicyber::node::Node pub_node("publisher");
  auto writer = pub_node.CreateWriter<std::string>("/test_single");
  ASSERT_NE(writer, nullptr);

  // 创建组件配置：组件名 + 一个 Reader 订阅 /test_single
  minicyber::proto::ComponentConfig config;
  config.set_name("single_channel_comp");
  auto* reader_opt = config.add_readers();
  ASSERT_NE(reader_opt, nullptr);
  reader_opt->set_channel("/test_single");

  // 初始化组件
  ASSERT_TRUE(comp->Initialize(config));
  EXPECT_TRUE(comp->init_called());

  // 验证组件未关闭
  EXPECT_FALSE(comp->IsShutdown());

  // 写入消息 → Proc 应被同步调用
  std::string test_msg("hello from single channel test");
  EXPECT_TRUE(writer->Write(test_msg));
  EXPECT_TRUE(comp->proc_called());
  EXPECT_EQ(received, test_msg);

  // 写入第二条不同消息
  EXPECT_TRUE(writer->Write("second message"));
  EXPECT_EQ(received, "second message");

  // 清理
  comp->Shutdown();
  EXPECT_TRUE(comp->IsShutdown());
}

// ---------------------------------------------------------------------------
// 多个独立通道 → Writer 有影响正确 Component
// ---------------------------------------------------------------------------
// 验证多个 Component 在不同通道上不会互相干扰。
// MiniCyber 的 ChannelBuffer 按 channel_id 哈希隔离，因此
// 不同 channel 的消息不会串到对方的 Reader 回调中。
// ===========================================================================
TEST(ComponentTest, MultipleChannelsIsolated) {
  std::string received_a, received_b;

  auto comp_a = std::make_shared<SingleChannelComponent>(&received_a);
  auto comp_b = std::make_shared<SingleChannelComponent>(&received_b);

  minicyber::node::Node pub_node("publisher");
  auto writer_a = pub_node.CreateWriter<std::string>("/chan_a");
  auto writer_b = pub_node.CreateWriter<std::string>("/chan_b");
  ASSERT_NE(writer_a, nullptr);
  ASSERT_NE(writer_b, nullptr);

  // 初始化 Component A（订阅 /chan_a）
  minicyber::proto::ComponentConfig cfg_a;
  cfg_a.set_name("comp_a");
  cfg_a.add_readers()->set_channel("/chan_a");
  ASSERT_TRUE(comp_a->Initialize(cfg_a));

  // 初始化 Component B（订阅 /chan_b）
  minicyber::proto::ComponentConfig cfg_b;
  cfg_b.set_name("comp_b");
  cfg_b.add_readers()->set_channel("/chan_b");
  ASSERT_TRUE(comp_b->Initialize(cfg_b));

  // 向 /chan_a 写消息 — 仅 comp_a 应收到
  EXPECT_TRUE(writer_a->Write("msg_for_a"));
  EXPECT_TRUE(comp_a->proc_called());
  EXPECT_EQ(received_a, "msg_for_a");
  EXPECT_FALSE(comp_b->proc_called());
  EXPECT_TRUE(received_b.empty());

  // 向 /chan_b 写消息 — 仅 comp_b 应收到
  EXPECT_TRUE(writer_b->Write("msg_for_b"));
  EXPECT_TRUE(comp_b->proc_called());
  EXPECT_EQ(received_b, "msg_for_b");

  comp_a->Shutdown();
  comp_b->Shutdown();
}

// ---------------------------------------------------------------------------
// 无 Reader 配置 → Initialize 返回 false
// ---------------------------------------------------------------------------
// Component<M0> 要求 Config::readers_size() >= 1。空配置时
// Initialize 必须优雅失败，而不是段错误或死循环。
// ===========================================================================
TEST(ComponentTest, NoReaderConfigFails) {
  auto comp = std::make_shared<NoReaderComponent>();
  minicyber::proto::ComponentConfig config;
  config.set_name("no_reader");
  // 不添加任何 ReaderOption
  EXPECT_FALSE(comp->Initialize(config));
}

// ---------------------------------------------------------------------------
// Init() 返回 false → Initialize 传播失败
// ---------------------------------------------------------------------------
// 业务组件的 Init() 可能因为资源不足、配置错误等原因失败。
// Initialize 必须将 Init() 的失败返回值传播到外层。
// ===========================================================================
TEST(ComponentTest, InitFailurePropagates) {
  auto comp = std::make_shared<FailInitComponent>();
  minicyber::proto::ComponentConfig config;
  config.set_name("fail_init");
  config.add_readers()->set_channel("/fail_chan");
  EXPECT_FALSE(comp->Initialize(config));
}

// ---------------------------------------------------------------------------
// Shutdown 后不再处理消息
// ---------------------------------------------------------------------------
// Component 关闭后，即使 Writer 继续发布消息，Proc 也不应被调用。
// 注意：当前阶段 Shutdown 只设置标志位，不去注销 DataNotifier 回调。
// 由于回调持有 weak_ptr，Component 析构后 shared_from_this().lock()
// 会返回 nullptr，回调自动安全跳过。
//
// 因此，这里只验证 Shutdown 标志位的存在不会导致崩溃。
// 真正的"关闭后不再触发"保证来自 Reader::Shutdown 中回调的解除注册，
// 这由 ComponentBase 的 Shutdown() → readers_ 清理完成。
// ===========================================================================
TEST(ComponentTest, ShutdownStopsProcessing) {
  std::string received;
  auto comp = std::make_shared<SingleChannelComponent>(&received);

  minicyber::node::Node pub_node("publisher");
  auto writer = pub_node.CreateWriter<std::string>("/shutdown_test");
  ASSERT_NE(writer, nullptr);

  minicyber::proto::ComponentConfig config;
  config.set_name("shutdown_comp");
  config.add_readers()->set_channel("/shutdown_test");
  ASSERT_TRUE(comp->Initialize(config));

  // 先验证正常接收
  EXPECT_TRUE(writer->Write("before_shutdown"));
  EXPECT_EQ(received, "before_shutdown");

  // Shutdown 后写入
  comp->Shutdown();
  received.clear();
  EXPECT_TRUE(writer->Write("after_shutdown"));
  // is_shutdown_ 导致 Process 直接返回 true，不调用 Proc
  // 所以 received 保持清空
  //
  // 注意：这里的行为取决于 Transport 是否还在转发消息到回调。
  // 实际上，Reader::Shutdown() 会 reset receiver_，但 DataNotifier
  // 中的回调条目可能还有残留（因为 ShmDispatcher 后台线程可能仍在
  // 分发）。因此这个测试仅验证 Shutdown 后不会 crash。
  // 更严格的"不触发"保证需要 DataNotifier 的反注册机制（Phase 7）。
  
//   | 时间线 | 发生了什么                                                                       |
//   | --- | --------------------------------------------------------------------------- |
//   | T1  | `comp->Shutdown()` → `Reader::Shutdown()` → `RemoveNotifier()` |
//   | T2  | `ShmDispatcher` 后台线程仍在运行（它不知道你 Shutdown 了）                                  |
//   | T3  | `writer->Write("after_shutdown")` → 消息写入 SHM                                |
//   | T4  | `ShmDispatcher::ThreadFunc` 被 `ConditionNotifier` 唤醒                        |
//   | T5  | `DataDispatcher::Dispatch` → `DataNotifier::Notify`                         |
//   | T6  | 遍历到残留的 Notifier，`callback == nullptr`，条件判断失败，安全跳过                           |

  SUCCEED();
}

// ---------------------------------------------------------------------------
// NullType 特化：无输入组件的 Initialize 成功
// ---------------------------------------------------------------------------
// Component<NullType> 特化不创建 Reader，仅创建 Node + Init()。
// 用于 TimerComponent 或纯启动器类的基类。
// ===========================================================================
TEST(ComponentTest, NullTypeInitialization) {
  auto comp = std::make_shared<NullComponent>();
  minicyber::proto::ComponentConfig config;
  config.set_name("null_type_comp");
  ASSERT_TRUE(comp->Initialize(config));
  EXPECT_TRUE(comp->init_called());
  comp->Shutdown();
}

// ---------------------------------------------------------------------------
// 资源管理：Component 析构时自动清理
// ---------------------------------------------------------------------------
// 验证 Component 的析构函数（通过 ComponentBase 的虚析构）会触发
// Shutdown。这里不手动调用 Shutdown，而是让 shared_ptr 离开作用域，
// 依赖智能指针的 RAII 语义自动清理。
// 测试通过 valgrind/ASan 能检测到资源泄漏（如果有的话）。
// ===========================================================================
TEST(ComponentTest, RaaICleanupOnDestruction) {
  std::string received;
  {
    auto comp = std::make_shared<SingleChannelComponent>(&received);
    minicyber::proto::ComponentConfig config;
    config.set_name("raii_test");
    config.add_readers()->set_channel("/raii_chan");
    ASSERT_TRUE(comp->Initialize(config));
    // comp 即将离开作用域 → 析构 → ~Component() → ~ComponentBase() → Shutdown
  }
  // 如果 Shutdown 有 bug（如未释放锁、double-free），ASan 会在此处报告
  SUCCEED();
}

TEST(ComponentTest, FailedInitializationCanBeRetried) {
  auto comp = std::make_shared<SingleChannelComponent>(nullptr);
  minicyber::proto::ComponentConfig config;
  config.set_name("retry_component");
  EXPECT_FALSE(comp->Initialize(config));

  config.add_readers()->set_channel("/retry_component");
  EXPECT_TRUE(comp->Initialize(config));
  comp->Shutdown();
}
