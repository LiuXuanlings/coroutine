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

// ChoreographyTaskConf 与原生 ChoreographyTask 对齐。processor_id 未配置或
// 越出定向区时由 Scheduler 放入 Classic 公共池，不创建第二套公共队列。
struct ChoreographyTaskConf {
  std::string name;
  uint32_t priority = 1;
  int processor_id = -1;
  bool has_processor = false;
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
// classic_groups 是  的正式 Classic 输入，由 Scheduler Protobuf 转换
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
  std::string choreography_affinity;
  std::vector<int> choreography_cpuset;
  std::string choreography_processor_policy = "SCHED_OTHER";
  int choreography_processor_prio = 0;
  uint32_t pool_processor_num = 0;
  std::string pool_affinity;
  std::vector<int> pool_cpuset;
  std::string pool_processor_policy = "SCHED_OTHER";
  int pool_processor_prio = 0;
  std::vector<ChoreographyTaskConf> choreography_tasks;
  std::vector<uint32_t> prio_threshold;

  // Classic 与 Choreography 分别按其原生配置职责转换；双区数量和 cpuset
  // 在  入口校验，避免启动后才暴露不可路由的任务。
  static bool FromProto(const ::minicyber::proto::SchedulerConf& proto,
                        SchedulerConf* conf);
};

}  // namespace scheduler
}  // namespace minicyber

#endif  // MINICYBER_SCHEDULER_SCHEDULER_CONF_H_
