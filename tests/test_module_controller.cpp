#include <gtest/gtest.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <google/protobuf/text_format.h>

#include "minicyber/mainboard/module_controller.h"
#include "minicyber/proto/dag_conf.pb.h"

#ifndef MINICYBER_TEST_PLUGIN_PATH
#error "MINICYBER_TEST_PLUGIN_PATH must be provided by CMake"
#endif

#ifndef MINICYBER_MAINBOARD_PATH
#error "MINICYBER_MAINBOARD_PATH must be provided by CMake"
#endif

namespace {

using minicyber::mainboard::ModuleController;
using minicyber::proto::DagConfig;

constexpr char kPluginClass[] = "Mc613PluginComponent";

std::string WriteTempFile(const std::string& suffix, const std::string& content) {
  std::string pattern = "/tmp/minicyber_mc613_XXXXXX" + suffix;
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const int fd = ::mkstemps(writable.data(), static_cast<int>(suffix.size()));
  if (fd < 0) return "";
  ::close(fd);
  const std::string path(writable.data());
  std::ofstream output(path);
  output << content;
  return path;
}

std::string ReadFile(const std::string& path) {
  std::ifstream input(path);
  return std::string((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
}

DagConfig MakeDag(const std::string& library, const std::string& class_name,
                  const std::string& node_name) {
  DagConfig dag;
  auto* module = dag.add_module_config();
  module->set_module_library(library);
  auto* component = module->add_components();
  component->set_class_name(class_name);
  component->mutable_config()->set_name(node_name);
  component->mutable_config()->add_readers()->set_channel("/mc613/test");
  return dag;
}

bool WaitForPluginInitialization(pid_t child, const std::string& trace) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    int status = 0;
    const pid_t result = ::waitpid(child, &status, WNOHANG);
    if (result == child) return false;
    if (ReadFile(trace).find('I') != std::string::npos) return true;
    ::usleep(10 * 1000);
  }
  return true;
}

int WaitForExit(pid_t child) {
  int status = 0;
  EXPECT_EQ(::waitpid(child, &status, 0), child);
  return status;
}

}  // namespace

TEST(ModuleControllerTest, RejectsEmptyModuleLibrary) {
  ModuleController controller({});
  EXPECT_FALSE(controller.LoadModule(MakeDag("", kPluginClass, "empty_library")));
  EXPECT_EQ(controller.ComponentCount(), 0u);
  EXPECT_EQ(controller.LibraryCount(), 0u);
}

TEST(ModuleControllerTest, DlopenRegistersComponentAndClearOrdersShutdownDestroyUnload) {
  const std::string trace = WriteTempFile(".trace", "");
  ASSERT_FALSE(trace.empty());
  ASSERT_EQ(::setenv("MINICYBER_MC613_PLUGIN_TRACE", trace.c_str(), 1), 0);

  ModuleController controller({});
  ASSERT_TRUE(controller.LoadModule(
      MakeDag(MINICYBER_TEST_PLUGIN_PATH, kPluginClass, "plugin_node")));
  EXPECT_EQ(controller.ComponentCount(), 1u);
  EXPECT_EQ(controller.LibraryCount(), 1u);
  EXPECT_EQ(ReadFile(trace), "I");

  controller.Clear();
  EXPECT_EQ(controller.ComponentCount(), 0u);
  EXPECT_EQ(controller.LibraryCount(), 0u);
  EXPECT_EQ(ReadFile(trace), "ISDU");
  ::unsetenv("MINICYBER_MC613_PLUGIN_TRACE");
  ::unlink(trace.c_str());
}

TEST(ModuleControllerTest, FailedLoadRollsBackOnlyCurrentWatermark) {
  const std::string trace = WriteTempFile(".trace", "");
  ASSERT_FALSE(trace.empty());
  ASSERT_EQ(::setenv("MINICYBER_MC613_PLUGIN_TRACE", trace.c_str(), 1), 0);

  ModuleController controller({});
  ASSERT_TRUE(controller.LoadModule(
      MakeDag(MINICYBER_TEST_PLUGIN_PATH, kPluginClass, "kept_component")));
  EXPECT_FALSE(controller.LoadModule(
      MakeDag(MINICYBER_TEST_PLUGIN_PATH, "UnknownPluginClass", "bad_component")));
  EXPECT_EQ(controller.ComponentCount(), 1u);
  EXPECT_EQ(controller.LibraryCount(), 1u);
  EXPECT_EQ(ReadFile(trace), "I");

  controller.Clear();
  EXPECT_EQ(ReadFile(trace), "ISDU");
  ::unsetenv("MINICYBER_MC613_PLUGIN_TRACE");
  ::unlink(trace.c_str());
}

TEST(ModuleControllerTest, LoadAllRejectsMissingLibraryAndRollsBack) {
  const std::string dag_path = WriteTempFile(
      ".dag", "module_config { components { class_name: \"" +
                  std::string(kPluginClass) +
                  "\" config { name: \"missing_library\" readers { channel: \"/x\" } } } }\n");
  ASSERT_FALSE(dag_path.empty());
  ModuleController controller({dag_path});
  EXPECT_FALSE(controller.LoadAll());
  EXPECT_EQ(controller.ComponentCount(), 0u);
  EXPECT_EQ(controller.LibraryCount(), 0u);
  ::unlink(dag_path.c_str());
}

TEST(MainboardTest, UsesOneDagWithClassicAndChoreographyConfigurations) {
  const std::string dag_path = WriteTempFile(
      ".dag", "module_config { module_library: \"" +
                  std::string(MINICYBER_TEST_PLUGIN_PATH) + "\" components { class_name: \"" +
                  std::string(kPluginClass) +
                  "\" config { name: \"mainboard_plugin\" readers { channel: \"/x\" } } } }\n");
  const std::string classic_path = WriteTempFile(
      ".conf", "policy: \"classic\" classic_conf { groups { name: \"default_grp\" processor_num: 1 } }\n");
  const std::string choreography_path = WriteTempFile(
      ".conf", "policy: \"choreography\" choreography_conf { choreography_processor_num: 1 pool_processor_num: 1 }\n");
  ASSERT_FALSE(dag_path.empty());
  ASSERT_FALSE(classic_path.empty());
  ASSERT_FALSE(choreography_path.empty());

  for (const std::string* config : {&classic_path, &choreography_path}) {
    const std::string trace = WriteTempFile(".trace", "");
    ASSERT_FALSE(trace.empty());
    const pid_t child = ::fork();
    ASSERT_NE(child, -1);
    if (child == 0) {
      ::setenv("MINICYBER_MC613_PLUGIN_TRACE", trace.c_str(), 1);
      ::execl(MINICYBER_MAINBOARD_PATH, MINICYBER_MAINBOARD_PATH, "-d",
              dag_path.c_str(), "-s", config->c_str(), nullptr);
      _exit(127);
    }
    ASSERT_TRUE(WaitForPluginInitialization(child, trace));
    ASSERT_EQ(::kill(child, SIGTERM), 0);
    const int status = WaitForExit(child);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(ReadFile(trace), "ISDU");
    ::unlink(trace.c_str());
  }
  ::unlink(dag_path.c_str());
  ::unlink(classic_path.c_str());
  ::unlink(choreography_path.c_str());
}
