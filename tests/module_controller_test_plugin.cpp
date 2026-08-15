#include <cstdlib>
#include <csignal>
#include <fstream>
#include <string>

#include "minicyber/component/component_base.h"
#include "minicyber/component/component_factory.h"

namespace {

void Trace(char event) {
  const char* path = std::getenv("MINICYBER_MC613_PLUGIN_TRACE");
  if (path != nullptr && path[0] != '\0') {
    std::ofstream output(path, std::ios::app);
    output << event;
  }
}

class Mc613PluginComponent : public minicyber::component::ComponentBase {
 public:
  ~Mc613PluginComponent() override { Trace('D'); }

  bool Initialize(const minicyber::proto::ComponentConfig& config) override {
    if (config.name().empty()) return false;
    Trace('I');
    if (std::getenv("MINICYBER_MC613_SIGNAL_DURING_INIT") != nullptr) {
      ::raise(SIGTERM);
    }
    return true;
  }

  void Shutdown() override {
    if (!IsShutdown()) Trace('S');
    ComponentBase::Shutdown();
  }

 protected:
  bool Init() override { return true; }
};

struct UnloadTrace {
  ~UnloadTrace() { Trace('U'); }
};

UnloadTrace g_unload_trace;
MINICYBER_REGISTER_COMPONENT(Mc613PluginComponent)

}  // namespace
