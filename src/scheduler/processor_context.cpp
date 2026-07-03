#include "minicyber/scheduler/processor_context.h"

namespace minicyber {
namespace scheduler {

void ProcessorContext::Shutdown() {
  stop_.store(true, std::memory_order_release);
}

}  // namespace scheduler
}  // namespace minicyber
