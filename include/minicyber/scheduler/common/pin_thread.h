#ifndef MINICYBER_SCHEDULER_COMMON_PIN_THREAD_H_
#define MINICYBER_SCHEDULER_COMMON_PIN_THREAD_H_

#include <string>
#include <thread>
#include <vector>

namespace minicyber {
namespace scheduler {

// 解析 cpuset 字符串为 CPU 编号列表。
// 支持逗号分隔的范围语法，如 "0-1,2-3" -> [0,1,2,3]。
// 非法格式抛 std::invalid_argument。
void ParseCpuset(const std::string& str, std::vector<int>* cpuset);

// 设置线程的 CPU 亲和性。
//   affinity == "range": 线程可在 cpus 列表中的任意 CPU 上运行
//   affinity == "1to1":  线程绑定到 cpus[cpu_id] 单个 CPU
//   cpu_id: 仅 1to1 模式使用；无有效索引时不修改亲和性
// 返回 true 表示无需设置或系统调用成功；配置无效、线程已退出
// 或宿主拒绝设置时返回 false，供 Scheduler 输出可诊断证据。
bool SetSchedAffinity(std::thread* thread, const std::vector<int>& cpus,
                      const std::string& affinity, int cpu_id = -1);

// 设置线程的调度策略。
//   spolicy == "SCHED_FIFO":  实时先进先出
//   spolicy == "SCHED_RR":    实时轮转
//   spolicy == "SCHED_OTHER": 普通分时（通过 nice 值调整优先级）
//   sched_priority: 优先级（实时策略 1-99，SCHED_OTHER 为 nice 值）
//   tid: 线程的 Linux TID，仅 SCHED_OTHER 使用，默认 -1 表示取调用线程
void SetSchedPolicy(std::thread* thread, const std::string& spolicy,
                    int sched_priority, pid_t tid = -1);

}  // namespace scheduler
}  // namespace minicyber

#endif  // MINICYBER_SCHEDULER_COMMON_PIN_THREAD_H_
