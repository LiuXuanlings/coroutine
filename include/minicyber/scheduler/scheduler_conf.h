#ifndef MINICYBER_SCHEDULER_SCHEDULER_CONF_H_
#define MINICYBER_SCHEDULER_SCHEDULER_CONF_H_

#include <cstdint>
#include <string>
#include <vector>

namespace minicyber {
namespace proto {
class SchedulerConf;
}  // namespace proto

namespace scheduler {

// ClassicTaskConf 与 scheduler_conf.proto 的 ClassicTask 一一对应。任务名命中
// 时由所属组决定队列和 Processor 集合，调用方传入的优先级不覆盖配置契约。
struct ClassicTaskConf {
  std::string name;
  uint32_t priority = 1;
  std::string group_name;
};

// ClassicGroupConf 保留 CyberRT SchedGroup 的职责：一个组拥有一套共享
// 20 级队列，组内多个 Processor 共同消费；它不是每个 Processor 的私有队列。
struct ClassicGroupConf {
  std::string name = "default_grp";
  uint32_t processor_num = 0;
  std::string affinity;
  std::vector<int> cpuset;
  std::string processor_policy = "SCHED_OTHER";
  int processor_prio = 0;
  std::vector<ClassicTaskConf> tasks;
};

// ----------------------------------------------------------------------
// SchedulerConf: 调度器配置
// ----------------------------------------------------------------------
// classic_groups 是 MC-610 的正式 Classic 输入，由 Scheduler Protobuf 转换
// 而来。未提供 group 时，旧字段只构造一个 default_grp，保持已存在调用方
// 的兼容；它不再表达每 Processor 一个 proc_i 组。
// ----------------------------------------------------------------------
struct SchedulerConf {
  uint32_t thread_num = 0;
  std::string policy = "classic";
  std::string affinity;  // "range" | "1to1" | ""
  std::vector<int> cpuset;
  std::string processor_policy = "SCHED_OTHER";
  int processor_prio = 0;
  std::vector<ClassicGroupConf> classic_groups;
  uint32_t choreography_processor_num = 0;
  std::vector<uint32_t> prio_threshold;

  // 只转换 MC-610 已接收的 Classic 字段。Choreography 的双区配置仍由
  // MC-611 接管，避免在 Classic 任务中提前改变其路由语义。
  static bool FromProto(const ::minicyber::proto::SchedulerConf& proto,
                        SchedulerConf* conf);
};

}  // namespace scheduler
}  // namespace minicyber

#endif  // MINICYBER_SCHEDULER_SCHEDULER_CONF_H_
