#include "minicyber/scheduler/scheduler_factory.h"

namespace minicyber {
namespace scheduler {

std::unique_ptr<Scheduler> SchedulerFactory::Create(const SchedulerConf& conf) {
  // 对齐 CyberRT SchedulerFactory 的策略白名单；错误策略不能静默降级为
  // Classic，否则会把 Choreography 配置错误伪装成可运行的另一种调度语义。
  if (conf.policy != "classic" && conf.policy != "choreography") {
    return nullptr;
  }
  // 双区是 Choreography 的完整生命周期边界；空定向区或空公共池会让任务
  // 在启动后才发现无可用路由，因此按验收口径在工厂入口明确拒绝。
  if (conf.policy == "choreography" &&
      (conf.choreography_processor_num == 0 || conf.pool_processor_num == 0)) {
    return nullptr;
  }
  return std::make_unique<Scheduler>(conf);
}

}  // namespace scheduler
}  // namespace minicyber
