#ifndef MINICYBER_DEMO_AUTODRIVE_METRICS_H_
#define MINICYBER_DEMO_AUTODRIVE_METRICS_H_

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace minicyber {
namespace autodrive {

// ControlSink 仅在 --metrics 开启时保留逐消息延迟。序列集合始终用于
// 主干完整性断言；这与 cyber_ref 的运行时观测职责相同，不应让关闭指标
// 的运行承担分位数样本的内存和排序成本。
struct Metrics {
  void RecordMeasurement(uint64_t sequence, uint64_t source_monotonic_ns,
                         uint64_t receive_monotonic_ns, bool collect_latency);
  void RecordAudit(uint64_t sequence);

  uint64_t received = 0;
  uint64_t audit_received = 0;
  uint64_t last_first_sequence = 0;
  uint64_t duplicates = 0;
  uint64_t audit_duplicates = 0;
  uint64_t out_of_order = 0;
  uint64_t first_receive_ns = 0;
  uint64_t last_receive_ns = 0;
  std::unordered_set<uint64_t> sequences;
  std::unordered_set<uint64_t> audit_sequences;
  std::vector<uint64_t> latency_ns;
};

}  // namespace autodrive
}  // namespace minicyber

#endif  // MINICYBER_DEMO_AUTODRIVE_METRICS_H_
