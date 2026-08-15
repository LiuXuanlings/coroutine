#include "demo/autodrive/evidence.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include <unistd.h>

namespace minicyber {
namespace autodrive {
namespace {

struct EvidenceState {
  std::mutex mutex;
  std::map<std::string, std::set<uint64_t>> component_sequences;
  std::map<std::string, pid_t> component_tids;
  std::map<uint64_t, uintptr_t> control_pointers;
  std::map<uint64_t, uintptr_t> audit_pointers;
  pid_t choreography_tid = -1;
  pid_t pool_tid = -1;
  uint64_t intra_enabled_count = 0;
  uint64_t shm_enabled_count = 0;
};

EvidenceState& State() {
  static EvidenceState state;
  return state;
}

const char* EvidencePath() { return std::getenv("MINICYBER_AUTODRIVE_EVIDENCE_FILE"); }

bool Enabled() {
  const char* path = EvidencePath();
  return path != nullptr && path[0] != '\0';
}

std::string JoinSequences(const std::set<uint64_t>& sequences) {
  std::ostringstream output;
  bool first = true;
  for (const uint64_t sequence : sequences) {
    if (!first) output << ',';
    output << sequence;
    first = false;
  }
  return output.str();
}

}  // namespace

void RuntimeEvidence::RecordComponent(const std::string& component,
                                      uint64_t sequence, pid_t tid,
                                      pid_t choreography_tid, pid_t pool_tid) {
  if (!Enabled() || sequence == 0) return;
  EvidenceState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.component_sequences[component].insert(sequence);
  state.component_tids.emplace(component, tid);
  state.choreography_tid = choreography_tid;
  state.pool_tid = pool_tid;
}

void RuntimeEvidence::RecordControl(uint64_t sequence, uintptr_t pointer,
                                    bool intra_enabled, bool shm_enabled) {
  if (!Enabled() || sequence == 0) return;
  EvidenceState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.control_pointers.emplace(sequence, pointer);
  if (intra_enabled) ++state.intra_enabled_count;
  if (shm_enabled) ++state.shm_enabled_count;
}

void RuntimeEvidence::RecordAudit(uint64_t sequence, uintptr_t pointer) {
  if (!Enabled() || sequence == 0) return;
  EvidenceState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.audit_pointers.emplace(sequence, pointer);
}

void RuntimeEvidence::Flush() {
  if (!Enabled()) return;
  EvidenceState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  std::ofstream output(EvidencePath());
  if (!output.is_open()) return;

  uint64_t pointer_identity_count = 0;
  for (const auto& control : state.control_pointers) {
    const auto audit = state.audit_pointers.find(control.first);
    if (audit != state.audit_pointers.end() && audit->second == control.second) {
      ++pointer_identity_count;
    }
  }

  output << "mainboard_pid=" << ::getpid() << '\n';
  output << "choreography_processor_0_tid=" << state.choreography_tid << '\n';
  output << "pool_processor_0_tid=" << state.pool_tid << '\n';
  for (const char* component : {"perception", "fusion", "planning", "control",
                                "control_audit"}) {
    output << "component_" << component << "_sequences="
           << JoinSequences(state.component_sequences[component]) << '\n';
    output << "component_" << component << "_tid="
           << state.component_tids[component] << '\n';
  }
  output << "control_sequences=" << JoinSequences([&] {
    std::set<uint64_t> sequences;
    for (const auto& item : state.control_pointers) sequences.insert(item.first);
    return sequences;
  }()) << '\n';
  output << "control_audit_sequences=" << JoinSequences([&] {
    std::set<uint64_t> sequences;
    for (const auto& item : state.audit_pointers) sequences.insert(item.first);
    return sequences;
  }()) << '\n';
  output << "control_intra_enabled_count=" << state.intra_enabled_count << '\n';
  output << "control_shm_enabled_count=" << state.shm_enabled_count << '\n';
  output << "intra_pointer_identity_count=" << pointer_identity_count << '\n';
  output << "control_first_pointer="
         << (state.control_pointers.empty() ? 0 : state.control_pointers.begin()->second)
         << '\n';
  output << "audit_first_pointer="
         << (state.audit_pointers.empty() ? 0 : state.audit_pointers.begin()->second)
         << '\n';
}

}  // namespace autodrive
}  // namespace minicyber
