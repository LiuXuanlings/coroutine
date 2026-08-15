#ifndef MINICYBER_SCHEDULER_SCHEDULER_FACTORY_H_
#define MINICYBER_SCHEDULER_SCHEDULER_FACTORY_H_

#include <memory>

#include "minicyber/scheduler/scheduler.h"

namespace minicyber {
namespace scheduler {

class SchedulerFactory {
 public:
  static std::unique_ptr<Scheduler> Create(const SchedulerConf& conf);
};

}  // namespace scheduler
}  // namespace minicyber

#endif  // MINICYBER_SCHEDULER_SCHEDULER_FACTORY_H_
