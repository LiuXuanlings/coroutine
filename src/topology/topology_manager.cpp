#include "minicyber/topology/topology_manager.h"

#include <sstream>

namespace minicyber {
namespace topology {

void TopologyManager::AddNode(const std::string& node_name, pid_t pid) {
  std::lock_guard<std::mutex> lg(mutex_);
  nodes_[node_name] = pid;
}

void TopologyManager::AddRole(std::vector<RoleAttr>& vec,
                              const std::string& node_name, pid_t pid) {
  for (const auto& r : vec) {
    if (r.node_name == node_name && r.pid == pid) return;  // 幂等
  }
  vec.push_back({node_name, pid});
}

void TopologyManager::AddChannelWriter(const std::string& channel,
                                       const std::string& node_name,
                                       pid_t pid) {
  std::lock_guard<std::mutex> lg(mutex_);
  nodes_[node_name] = pid;
  AddRole(channels_[channel].writers, node_name, pid);
}

void TopologyManager::AddChannelReader(const std::string& channel,
                                       const std::string& node_name,
                                       pid_t pid) {
  std::lock_guard<std::mutex> lg(mutex_);
  nodes_[node_name] = pid;
  AddRole(channels_[channel].readers, node_name, pid);
}

bool TopologyManager::HasNode(const std::string& node_name, pid_t pid) const {
  std::lock_guard<std::mutex> lg(mutex_);
  auto it = nodes_.find(node_name);
  return it != nodes_.end() && it->second == pid;
}

bool TopologyManager::IsSameProc(const std::string& channel) const {
  std::lock_guard<std::mutex> lg(mutex_);
  auto it = channels_.find(channel);
  if (it == channels_.end()) return false;
  const auto& info = it->second;
  if (info.writers.empty() || info.readers.empty()) return false;

  pid_t pid = info.writers[0].pid;
  for (const auto& w : info.writers) {
    if (w.pid != pid) return false;
  }
  for (const auto& r : info.readers) {
    if (r.pid != pid) return false;
  }
  return true;
}

Relation TopologyManager::GetRelation(const std::string& channel,
                                      pid_t pid) const {
  std::lock_guard<std::mutex> lg(mutex_);
  auto it = channels_.find(channel);
  if (it == channels_.end()) return NO_RELATION;
  const auto& info = it->second;
  if (info.writers.empty() || info.readers.empty()) return NO_RELATION;

  bool has_cross = false;
  for (const auto& w : info.writers) {
    if (w.pid != pid) has_cross = true;
  }
  for (const auto& r : info.readers) {
    if (r.pid != pid) has_cross = true;
  }
  return has_cross ? DIFF_PROC : SAME_PROC;
}

std::string TopologyManager::DumpGraph() const {
  std::lock_guard<std::mutex> lg(mutex_);
  std::ostringstream oss;
  oss << "digraph minicyber_topology {\n";
  for (const auto& kv : channels_) {
    const auto& channel = kv.first;
    const auto& info = kv.second;
    for (const auto& w : info.writers) {
      for (const auto& r : info.readers) {
        oss << "  \"" << w.node_name << "\" -> \"" << r.node_name
            << "\" [label=\"" << channel << "\"];\n";
      }
    }
  }
  oss << "}\n";
  return oss.str();
}

void TopologyManager::Shutdown() {
  std::lock_guard<std::mutex> lg(mutex_);
  channels_.clear();
  nodes_.clear();
}

}  // namespace topology
}  // namespace minicyber