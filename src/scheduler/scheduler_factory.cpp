#include "minicyber/scheduler/scheduler_factory.h"

namespace minicyber {
namespace scheduler {

std::unique_ptr<Scheduler> SchedulerFactory::Create(const SchedulerConf& conf) {
  return std::make_unique<Scheduler>(conf);
}

}  // namespace scheduler
}  // namespace minicyber
