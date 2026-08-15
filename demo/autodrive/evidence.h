#ifndef MINICYBER_DEMO_AUTODRIVE_EVIDENCE_H_
#define MINICYBER_DEMO_AUTODRIVE_EVIDENCE_H_

#include <cstdint>
#include <string>

#include <sys/types.h>

namespace minicyber {
namespace autodrive {

// 业务取证只在设置 MINICYBER_AUTODRIVE_EVIDENCE_FILE 时记录。
// 记录留在常驻 Runtime 而非可卸载组件 DSO，保证 ControlComponent 发布的
// shared_ptr 与 ControlAudit 接收的同一对象可在 dlclose 前可靠比较。
class RuntimeEvidence final {
 public:
  static void RecordComponent(const std::string& component, uint64_t sequence,
                              pid_t tid, pid_t choreography_tid,
                              pid_t pool_tid);
  static void RecordControl(uint64_t sequence, uintptr_t pointer,
                            bool intra_enabled, bool shm_enabled);
  static void RecordAudit(uint64_t sequence, uintptr_t pointer);
  static void Flush();
};

}  // namespace autodrive
}  // namespace minicyber

#endif  // MINICYBER_DEMO_AUTODRIVE_EVIDENCE_H_
