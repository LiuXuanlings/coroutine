#include "demo/autodrive/metrics.h"

#include <algorithm>

namespace minicyber {
namespace autodrive {

void Metrics::RecordMeasurement(uint64_t sequence, uint64_t source_monotonic_ns,
                                uint64_t receive_monotonic_ns,
                                bool collect_latency) {
  if (received == 0) first_receive_ns = receive_monotonic_ns;
  last_receive_ns = receive_monotonic_ns;
  ++received;
  if (!sequences.insert(sequence).second) {
    ++duplicates;
  } else {
    if (last_first_sequence != 0 && sequence < last_first_sequence) {
      ++out_of_order;
    }
    last_first_sequence = std::max(last_first_sequence, sequence);
  }
  if (collect_latency && receive_monotonic_ns >= source_monotonic_ns) {
    latency_ns.push_back(receive_monotonic_ns - source_monotonic_ns);
  }
}

void Metrics::RecordAudit(uint64_t sequence) {
  ++audit_received;
  if (!audit_sequences.insert(sequence).second) ++audit_duplicates;
}

}  // namespace autodrive
}  // namespace minicyber
