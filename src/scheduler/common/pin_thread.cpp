#include "minicyber/scheduler/common/pin_thread.h"

#include <sched.h>
#include <sys/resource.h>
#include <stdexcept>
#include <sstream>
#include <cstring>

namespace minicyber {
namespace scheduler {

// ----------------------------------------------------------------------
// ParseCpuset: 解析 "0-1,2-3" 这种逗号分隔的范围语法
// ----------------------------------------------------------------------
// 示例：
//   "0"       -> [0]
//   "0-1"     -> [0, 1]
//   "0-1,2-3" -> [0, 1, 2, 3]
//
// 非法格式（如 "0-1-2"）抛 std::invalid_argument，替代原 CyberRT 的 exit(0)。
// ----------------------------------------------------------------------
void ParseCpuset(const std::string& str, std::vector<int>* cpuset) {
  std::stringstream ss(str);
  std::string token;
  // 从输入流读取字符存入str，读到delim分隔符停止，分隔符不存入str
  //std::getline(is, str, delim);
  while (std::getline(ss, token, ',')) {
    std::stringstream range_ss(token);
    std::string num_str;
    std::vector<int> range;
    while (std::getline(range_ss, num_str, '-')) {
      range.push_back(std::stoi(num_str));
    }
    if (range.size() == 1) {
      cpuset->push_back(range[0]);
    } else if (range.size() == 2) {
      for (int i = range[0]; i <= range[1]; ++i) {
        cpuset->push_back(i);
      }
    } else {
      throw std::invalid_argument("ParseCpuset: invalid range '" + token +
                                  "' in '" + str + "'");
    }
  }
}

// ----------------------------------------------------------------------
// SetSchedAffinity: 设置线程 CPU 亲和性
// ----------------------------------------------------------------------
// range 模式：线程可在 cpus 中任意 CPU 运行
// 1to1 模式：线程绑定到 cpus[cpu_id] 单核
// 依赖系统接口说明：
// 1. cpu_set_t
//    Linux独有CPU掩码位集，每一位对应一个CPU核心；bitN=1允许线程跑CPU N，bitN=0禁止；
//    仅能通过配套宏修改，不可直接手动赋值。
// 2. 掩码操作宏（头文件 <sched.h>）
//    CPU_ZERO(&set)：清空掩码，初始化全部位为0，设置亲和性前必调用
//    CPU_SET(cpu, &set)：将指定CPU核心加入可用掩码
//    拓展：CPU_CLR移除核心、CPU_ISSET查询核心是否启用
// 3. std::thread::native_handle()
//    获取底层原生线程句柄，Linux下返回 pthread_t；
//    pthread_setaffinity_np仅识别pthread_t，无法直接接收std::thread对象。
// 4. pthread_setaffinity_np
//    Linux GNU非标准扩展（_np=non-portable不可移植），Windows/macOS无该接口；
//    参数：thread(pthread_t句柄)、cpusetsize(sizeof(cpu_set_t))、cpuset(CPU掩码)
//    返回0代表设置成功，负数为失败，当前代码未处理返回错误。
// ----------------------------------------------------------------------
void SetSchedAffinity(std::thread* thread, const std::vector<int>& cpus,
                      const std::string& affinity, int cpu_id) {
  if (cpus.empty()) {
    return;
  }

  cpu_set_t set;
  CPU_ZERO(&set);

  if (affinity == "range") {
    for (const auto cpu : cpus) {
      CPU_SET(cpu, &set);
    }
    pthread_setaffinity_np(thread->native_handle(), sizeof(set), &set);
  } else if (affinity == "1to1") {
    if (cpu_id == -1 || static_cast<uint32_t>(cpu_id) >= cpus.size()) {
      return;
    }
    CPU_SET(cpus[cpu_id], &set);
    pthread_setaffinity_np(thread->native_handle(), sizeof(set), &set);
  }
}

// ----------------------------------------------------------------------
// SetSchedPolicy: 设置线程调度策略与优先级
// ----------------------------------------------------------------------
// 依赖系统接口说明：
// 1. sched_param
//    POSIX 调度参数结构体，sched_priority 字段表示静态优先级。
//    仅对实时策略（SCHED_FIFO/SCHED_RR）有效，取值 1-99；
//    SCHED_OTHER 必须设为 0。
//
// 2. pthread_setschedparam
//    POSIX 标准接口，设置线程的调度策略与参数。
//    需要 CAP_SYS_NICE 权限才能设置实时策略；
//    普通用户调用会返回 EPERM，属于预期行为。
//
// 3. setpriority(PRIO_PROCESS, tid, prio)
//    设置进程/线程的 nice 值（-20 到 19，值越低优先级越高）。
//    tid 为 Linux 内核线程 ID（通过 syscall(SYS_gettid) 获取），
//    非 pthread_t 也非 std::thread::get_id()。
// ----------------------------------------------------------------------
void SetSchedPolicy(std::thread* thread, const std::string& spolicy,
                    int sched_priority, pid_t tid) {
  struct sched_param sp;
  memset(reinterpret_cast<void*>(&sp), 0, sizeof(sp));
  sp.sched_priority = sched_priority;

  if (spolicy == "SCHED_FIFO") {
    pthread_setschedparam(thread->native_handle(), SCHED_FIFO, &sp);
  } else if (spolicy == "SCHED_RR") {
    pthread_setschedparam(thread->native_handle(), SCHED_RR, &sp);
  } else if (spolicy == "SCHED_OTHER") {
    // SCHED_OTHER 的优先级通过 nice 值调整，sched_priority 此时为 nice 值
    setpriority(PRIO_PROCESS, tid, sched_priority);
  }
}

}  // namespace scheduler
}  // namespace minicyber
