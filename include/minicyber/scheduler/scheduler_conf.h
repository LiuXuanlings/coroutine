#ifndef MINICYBER_SCHEDULER_SCHEDULER_CONF_H_
#define MINICYBER_SCHEDULER_SCHEDULER_CONF_H_

#include <cstdint>
#include <string>
#include <vector>

namespace minicyber {
namespace scheduler {

// ----------------------------------------------------------------------
// SchedulerConf: 调度器配置
// ----------------------------------------------------------------------
//   thread_num:     工作线程数，0 表示按 CPU 核数自动
//   policy:         调度策略，如 "classic"
//   affinity:       CPU 亲和性模式，"range" 或 "1to1"，空表示不绑核
//   cpuset:         可用的 CPU 编号列表
//   prio_threshold: 优先级阈值（保留，当前未使用）
// ----------------------------------------------------------------------
struct SchedulerConf {
  uint32_t thread_num = 0;
  std::string policy = "classic";
  std::string affinity;  // "range" | "1to1" | ""
  std::vector<int> cpuset;
  std::vector<uint32_t> prio_threshold;
};

}  // namespace scheduler
}  // namespace minicyber

#endif  // MINICYBER_SCHEDULER_SCHEDULER_CONF_H_
