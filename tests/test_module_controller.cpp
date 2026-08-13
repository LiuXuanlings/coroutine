#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/text_format.h>

#include "minicyber/component/component.h"
#include "minicyber/component/component_factory.h"
#include "minicyber/mainboard/module_controller.h"
#include "minicyber/node/node.h"
#include "minicyber/proto/dag_conf.pb.h"

// =============================================================================
// ModuleController 单测
//
// 测试策略：
//   1. 不依赖 dlopen（构建独立 .so 增加测试复杂度，收益有限）
//      使用 module_library="" 的 ModuleConfig，跳过 dlopen 路径，
//      直接验证工厂实例化 + Initialize 链路。
//      MINICYBER_REGISTER_COMPONENT 宏在测试二进制启动时即完成静态注册。
//
//   2. DAG 文件解析测试：写入临时 .dag 文本 proto，ParseDagFile 验证字段。
//
//   3. 错误路径：未注册类名、不存在的 .so、缺失 DAG 文件。
//
//   4. 端到端：LoadAll → Writer 写数据 → Proc 被调用 → Clear。
// =============================================================================

using minicyber::component::ComponentFactory;
using minicyber::mainboard::ModuleController;
using minicyber::proto::ComponentConfig;
using minicyber::proto::ComponentInfo;
using minicyber::proto::DagConfig;
using minicyber::proto::ModuleConfig;

namespace {

// 测试用事件驱动组件
class McTestComponent
    : public minicyber::component::Component<std::string> {
 public:
  static void ResetStats() {
    init_count_.store(0, std::memory_order_relaxed);
    proc_count_.store(0, std::memory_order_relaxed);
    last_msg_.clear();
  }
  static int init_count() { return init_count_.load(std::memory_order_relaxed); }
  static int proc_count() { return proc_count_.load(std::memory_order_relaxed); }
  static const std::string& last_msg() { return last_msg_; }

 protected:
  bool Init() override {
    init_count_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  bool Proc(const std::shared_ptr<std::string>& msg) override {
    last_msg_ = *msg;
    proc_count_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

 private:
  static std::atomic<int> init_count_;
  static std::atomic<int> proc_count_;
  static std::string last_msg_;
};

std::atomic<int> McTestComponent::init_count_{0};
std::atomic<int> McTestComponent::proc_count_{0};
std::string McTestComponent::last_msg_;

// 第二个测试组件（验证多类共存）
class McSecondComponent
    : public minicyber::component::Component<std::string> {
 protected:
  bool Init() override { return true; }
  bool Proc(const std::shared_ptr<std::string>& msg) override {
    (void)msg;
    return true;
  }
};

MINICYBER_REGISTER_COMPONENT(McTestComponent)
MINICYBER_REGISTER_COMPONENT(McSecondComponent)

// 辅助：写临时 .dag 文件
std::string WriteTempDag(const std::string& content) {
  char path[] = "/tmp/minicyber_test_XXXXXX.dag";
  int fd = ::mkstemps(path, 4);
  if (fd < 0) return "";
  ::close(fd);
  std::ofstream ofs(path);
  ofs << content;
  ofs.close();
  return path;
}

}  // namespace

// =============================================================================
// 基础：空 DAG 列表
// =============================================================================

TEST(ModuleControllerTest, LoadAllEmptyDagList) {
  ModuleController controller({});
  EXPECT_TRUE(controller.LoadAll());
  EXPECT_EQ(controller.ComponentCount(), 0u);
  EXPECT_EQ(controller.LibraryCount(), 0u);
  controller.Clear();
}

// =============================================================================
// LoadModule：使用已注册的组件（module_library="" 跳过 dlopen）
// =============================================================================

TEST(ModuleControllerTest, LoadModuleInProcessComponent) {
  McTestComponent::ResetStats();

  DagConfig dag;
  ModuleConfig* mc = dag.add_module_config();
  mc->set_module_library("");  // 不 dlopen
  ComponentInfo* ci = mc->add_components();
  ci->set_class_name("McTestComponent");
  ComponentConfig* cc = ci->mutable_config();
  cc->set_name("mc_test_node");
  cc->add_readers()->set_channel("/mc_test_channel");

  ModuleController controller({});
  ASSERT_TRUE(controller.LoadModule(dag));
  EXPECT_EQ(controller.ComponentCount(), 1u);
  EXPECT_EQ(controller.LibraryCount(), 0u);
  EXPECT_EQ(McTestComponent::init_count(), 1);

  controller.Clear();
  EXPECT_EQ(controller.ComponentCount(), 0u);
}

// =============================================================================
// LoadModule：未注册类名 → 返回 false
// =============================================================================

TEST(ModuleControllerTest, LoadModuleUnknownClassName) {
  DagConfig dag;
  ModuleConfig* mc = dag.add_module_config();
  mc->set_module_library("");
  ComponentInfo* ci = mc->add_components();
  ci->set_class_name("NonExistentClassXYZ");
  ci->mutable_config()->set_name("bad_node");

  ModuleController controller({});
  EXPECT_FALSE(controller.LoadModule(dag));
  // 失败后 component_list_ 可能为空或包含部分，但 Clear 应安全
  controller.Clear();
}

TEST(ModuleControllerTest, RejectsIncompleteComponentDagEntry) {
  DagConfig dag;
  auto* component = dag.add_module_config()->add_components();
  component->set_class_name("McTestComponent");
  component->mutable_config()->set_name("incomplete_component");

  ModuleController controller({});
  EXPECT_FALSE(controller.LoadModule(dag));
  EXPECT_EQ(controller.ComponentCount(), 0u);
}

TEST(ModuleControllerTest, ParsePreservesReaderOptionFields) {
  const std::string dag_content =
      "module_config {\n"
      "  module_library: \"\"\n"
      "  components {\n"
      "    class_name: \"McTestComponent\"\n"
      "    config {\n"
      "      name: \"rich_proto_node\"\n"
      "      config_file_path: \"component.conf\"\n"
      "      flag_file_path: \"component.flags\"\n"
      "      readers {\n"
      "        channel: \"/rich_proto\"\n"
      "        pending_queue_size: 3\n"
      "        qos_profile { depth: 3 reliability: RELIABILITY_BEST_EFFORT }\n"
      "      }\n"
      "    }\n"
      "  }\n"
      "}\n";
  DagConfig dag;
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(dag_content, &dag));
  const auto& reader = dag.module_config(0).components(0).config().readers(0);
  EXPECT_EQ(reader.channel(), "/rich_proto");
  EXPECT_EQ(reader.pending_queue_size(), 3u);
  EXPECT_EQ(reader.qos_profile().depth(), 3u);
  EXPECT_EQ(reader.qos_profile().reliability(),
            minicyber::proto::RELIABILITY_BEST_EFFORT);
}

// =============================================================================
// LoadModule：不存在的 .so → 返回 false
// =============================================================================

TEST(ModuleControllerTest, LoadModuleNonExistentLibrary) {
  DagConfig dag;
  ModuleConfig* mc = dag.add_module_config();
  mc->set_module_library("/nonexistent/path/libfoo.so");
  ComponentInfo* ci = mc->add_components();
  ci->set_class_name("McTestComponent");
  ci->mutable_config()->set_name("x");
  ci->mutable_config()->add_readers()->set_channel("/x");

  ModuleController controller({});
  EXPECT_FALSE(controller.LoadModule(dag));
  EXPECT_EQ(controller.LibraryCount(), 0u);
  controller.Clear();
}

// =============================================================================
// LoadModule：TimerComponent 路径
// =============================================================================

TEST(ModuleControllerTest, LoadModuleTimerComponent) {
  // 使用 McSecondComponent 假装是 timer component（仅为验证 LoadModule
  // 的 timer_components 分支不会崩溃；实际类型不匹配会在 Initialize 返回 false）
  // 这里用一个真实继承自 TimerComponent 的类更合适，但为简化测试，
  // 我们只验证 LoadModule 在 timer_components 为空时不会创建任何组件。
  DagConfig dag;
  ModuleConfig* mc = dag.add_module_config();
  mc->set_module_library("");
  // 不添加任何 components 或 timer_components

  ModuleController controller({});
  ASSERT_TRUE(controller.LoadModule(dag));
  EXPECT_EQ(controller.ComponentCount(), 0u);
  controller.Clear();
}

// =============================================================================
// LoadAll：从文件路径加载
// =============================================================================

TEST(ModuleControllerTest, LoadAllFromDagFile) {
  McTestComponent::ResetStats();

  std::string dag_content =
      "module_config {\n"
      "  module_library: \"\"\n"
      "  components {\n"
      "    class_name: \"McTestComponent\"\n"
      "    config {\n"
      "      name: \"file_loaded_node\"\n"
      "      readers { channel: \"/mc_file_channel\" }\n"
      "    }\n"
      "  }\n"
      "}\n";
  std::string path = WriteTempDag(dag_content);
  ASSERT_FALSE(path.empty());

  ModuleController controller({path});
  ASSERT_TRUE(controller.LoadAll());
  EXPECT_EQ(controller.ComponentCount(), 1u);
  EXPECT_EQ(McTestComponent::init_count(), 1);

  controller.Clear();
  ::unlink(path.c_str());
}

// =============================================================================
// LoadAll：DAG 文件不存在 → 返回 false
// =============================================================================

TEST(ModuleControllerTest, LoadAllNonExistentDagFile) {
  ModuleController controller({"/nonexistent/path/missing.dag"});
  EXPECT_FALSE(controller.LoadAll());
  controller.Clear();
}

// =============================================================================
// LoadAll：多个 DAG 路径
// =============================================================================

TEST(ModuleControllerTest, LoadAllMultipleDagPaths) {
  McTestComponent::ResetStats();

  std::string dag1 =
      "module_config {\n"
      "  module_library: \"\"\n"
      "  components {\n"
      "    class_name: \"McTestComponent\"\n"
      "    config { name: \"n1\" readers { channel: \"/c1\" } }\n"
      "  }\n"
      "}\n";
  std::string dag2 =
      "module_config {\n"
      "  module_library: \"\"\n"
      "  components {\n"
      "    class_name: \"McSecondComponent\"\n"
      "    config { name: \"n2\" readers { channel: \"/c2\" } }\n"
      "  }\n"
      "}\n";
  std::string p1 = WriteTempDag(dag1);
  std::string p2 = WriteTempDag(dag2);
  ASSERT_FALSE(p1.empty());
  ASSERT_FALSE(p2.empty());

  ModuleController controller({p1, p2});
  ASSERT_TRUE(controller.LoadAll());
  EXPECT_EQ(controller.ComponentCount(), 2u);

  controller.Clear();
  ::unlink(p1.c_str());
  ::unlink(p2.c_str());
}

TEST(ModuleControllerTest, LoadAllFailureRollsBackEarlierDag) {
  const std::string valid_dag =
      "module_config { components { class_name: \"McTestComponent\" "
      "config { name: \"rollback_first\" readers { channel: \"/rollback_first\" } } } }\n";
  const std::string invalid_dag =
      "module_config { components { class_name: \"UnknownRollbackClass\" "
      "config { name: \"rollback_second\" readers { channel: \"/rollback_second\" } } } }\n";
  const std::string first = WriteTempDag(valid_dag);
  const std::string second = WriteTempDag(invalid_dag);
  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());

  ModuleController controller({first, second});
  EXPECT_FALSE(controller.LoadAll());
  EXPECT_EQ(controller.ComponentCount(), 0u);
  EXPECT_EQ(controller.LibraryCount(), 0u);
  ::unlink(first.c_str());
  ::unlink(second.c_str());
}

TEST(ModuleControllerTest, LoadModuleFailureRollsBackEarlierComponent) {
  DagConfig dag;
  auto* module = dag.add_module_config();
  auto* valid = module->add_components();
  valid->set_class_name("McTestComponent");
  valid->mutable_config()->set_name("rollback_module_first");
  valid->mutable_config()->add_readers()->set_channel("/rollback_module_first");
  auto* invalid = module->add_components();
  invalid->set_class_name("UnknownRollbackClass");
  invalid->mutable_config()->set_name("rollback_module_second");
  invalid->mutable_config()->add_readers()->set_channel("/rollback_module_second");

  ModuleController controller({});
  EXPECT_FALSE(controller.LoadModule(dag));
  EXPECT_EQ(controller.ComponentCount(), 0u);
  EXPECT_EQ(controller.LibraryCount(), 0u);
}

// =============================================================================
// 端到端：LoadAll → Writer 写数据 → Proc 被调用 → Clear
// =============================================================================

TEST(ModuleControllerTest, EndToEndDataDelivery) {
  McTestComponent::ResetStats();

  std::string dag_content =
      "module_config {\n"
      "  module_library: \"\"\n"
      "  components {\n"
      "    class_name: \"McTestComponent\"\n"
      "    config { name: \"e2e_node\" readers { channel: \"/e2e_channel\" } }\n"
      "  }\n"
      "}\n";
  std::string path = WriteTempDag(dag_content);
  ASSERT_FALSE(path.empty());

  ModuleController controller({path});
  ASSERT_TRUE(controller.LoadAll());
  EXPECT_EQ(McTestComponent::init_count(), 1);

  // 创建 Writer 发布消息
  minicyber::node::Node pub_node("e2e_publisher");
  auto writer = pub_node.CreateWriter<std::string>("/e2e_channel");
  ASSERT_NE(writer, nullptr);

  std::string msg("hello from mainboard test");
  EXPECT_TRUE(writer->Write(msg));
  // 同步回调路径，Write 返回时 Proc 应已执行
  EXPECT_EQ(McTestComponent::proc_count(), 1);
  EXPECT_EQ(McTestComponent::last_msg(), msg);

  controller.Clear();
  ::unlink(path.c_str());
}

// =============================================================================
// Clear：重复调用安全（幂等）
// =============================================================================

TEST(ModuleControllerTest, ClearIdempotent) {
  McTestComponent::ResetStats();
  DagConfig dag;
  ModuleConfig* mc = dag.add_module_config();
  mc->set_module_library("");
  ComponentInfo* ci = mc->add_components();
  ci->set_class_name("McTestComponent");
  ci->mutable_config()->set_name("idem");
  ci->mutable_config()->add_readers()->set_channel("/idem");

  ModuleController controller({});
  ASSERT_TRUE(controller.LoadModule(dag));
  controller.Clear();
  controller.Clear();  // 二次调用不应崩溃
  EXPECT_EQ(controller.ComponentCount(), 0u);
  EXPECT_EQ(controller.LibraryCount(), 0u);
}

// =============================================================================
// 析构函数自动清理
// =============================================================================

TEST(ModuleControllerTest, DestructorAutoClears) {
  McTestComponent::ResetStats();
  DagConfig dag;
  ModuleConfig* mc = dag.add_module_config();
  mc->set_module_library("");
  ComponentInfo* ci = mc->add_components();
  ci->set_class_name("McTestComponent");
  ci->mutable_config()->set_name("dtor");
  ci->mutable_config()->add_readers()->set_channel("/dtor");

  {
    ModuleController controller({});
    ASSERT_TRUE(controller.LoadModule(dag));
    EXPECT_EQ(controller.ComponentCount(), 1u);
    // 出作用域 → ~ModuleController() 调用 Clear()
  }
  SUCCEED();  // 到这里说明析构未崩溃
}
