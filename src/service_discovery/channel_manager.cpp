#include "minicyber/service_discovery/channel_manager.h"

#include <functional>
#include <iterator>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace minicyber {
namespace service_discovery {
namespace {

uint64_t RoleKey(const proto::RoleAttributes& attr) {
  // 原生发现协议优先使用全局角色 id；兼容入口缺少 id 时才生成稳定键。
  if (attr.has_id()) {
    return attr.id();
  }
  return std::hash<std::string>{}(attr.node_name() + attr.channel_name()) ^
         static_cast<uint64_t>(attr.process_id());
}

}  // namespace

class ChannelManager::Impl {
 public:
  using RoleTable = std::unordered_map<
      std::string, std::unordered_map<uint64_t, proto::RoleAttributes>>;

  bool Apply(const proto::ChangeMsg& msg) {
    // Writer、Reader 和派生的 Node 状态在同一临界区提交，保证查询只看到
    // 完整变更。
    std::lock_guard<std::mutex> lock(mutex);
    auto& roles = msg.role_type() == proto::ROLE_WRITER ? writers : readers;
    const auto channel_name = msg.role_attr().channel_name();
    const auto key = RoleKey(msg.role_attr());
    if (msg.operate_type() == proto::OPT_JOIN) {
      auto& channel_roles = roles[channel_name];
      const bool changed = channel_roles.find(key) == channel_roles.end() ||
                           channel_roles[key].SerializeAsString() !=
                               msg.role_attr().SerializeAsString();
      channel_roles[key] = msg.role_attr();
      nodes[msg.role_attr().node_name()] = msg.role_attr().process_id();
      return changed;
    }

    const auto channel = roles.find(channel_name);
    if (channel == roles.end() || channel->second.erase(key) == 0) {
      return false;
    }
    if (channel->second.empty()) {
      roles.erase(channel);
    }
    RemoveNodeIfUnused(msg.role_attr().node_name(), msg.role_attr().process_id());
    return true;
  }

  std::vector<RemovedRole> RemoveProcess(const std::string& host_name,
                                         pid_t process_id) {
    std::vector<RemovedRole> removed;
    std::lock_guard<std::mutex> lock(mutex);
    RemoveProcessFromTable(&writers, proto::ROLE_WRITER, host_name, process_id,
                           &removed);
    RemoveProcessFromTable(&readers, proto::ROLE_READER, host_name, process_id,
                           &removed);
    for (auto node = nodes.begin(); node != nodes.end();) {
      node = node->second == process_id ? nodes.erase(node) : std::next(node);
    }
    return removed;
  }

  bool HasRole(const RoleTable& roles, const std::string& channel_name) const {
    std::lock_guard<std::mutex> lock(mutex);
    const auto channel = roles.find(channel_name);
    return channel != roles.end() && !channel->second.empty();
  }

  std::vector<proto::RoleAttributes> Snapshot(
      const RoleTable& roles, const std::string& channel_name) const {
    std::vector<proto::RoleAttributes> result;
    std::lock_guard<std::mutex> lock(mutex);
    const auto channel = roles.find(channel_name);
    if (channel == roles.end()) {
      return result;
    }
    for (const auto& role : channel->second) {
      result.push_back(role.second);
    }
    return result;
  }

  void RemoveNodeIfUnused(const std::string& node_name, pid_t process_id) {
    // 同一 Node 可同时拥有多个端点，最后一个端点 Leave 后才能移除 Node。
    auto contains_node = [&](const RoleTable& roles) {
      for (const auto& channel : roles) {
        for (const auto& role : channel.second) {
          if (role.second.node_name() == node_name &&
              role.second.process_id() == process_id) {
            return true;
          }
        }
      }
      return false;
    };
    if (!contains_node(writers) && !contains_node(readers)) {
      const auto node = nodes.find(node_name);
      if (node != nodes.end() && node->second == process_id) {
        nodes.erase(node);
      }
    }
  }

  static void RemoveProcessFromTable(RoleTable* roles,
                                     proto::RoleType role_type,
                                     const std::string& host_name,
                                     pid_t process_id,
                                     std::vector<RemovedRole>* removed) {
    for (auto channel = roles->begin(); channel != roles->end();) {
      for (auto role = channel->second.begin(); role != channel->second.end();) {
        if (role->second.host_name() == host_name &&
            role->second.process_id() == process_id) {
          removed->push_back({role_type, role->second});
          role = channel->second.erase(role);
        } else {
          ++role;
        }
      }
      channel = channel->second.empty() ? roles->erase(channel) : std::next(channel);
    }
  }

  mutable std::mutex mutex;
  RoleTable writers;
  RoleTable readers;
  std::unordered_map<std::string, pid_t> nodes;
};

ChannelManager::ChannelManager() : impl_(std::make_unique<Impl>()) {}
ChannelManager::~ChannelManager() = default;
bool ChannelManager::Apply(const proto::ChangeMsg& msg) { return impl_->Apply(msg); }
std::vector<ChannelManager::RemovedRole> ChannelManager::RemoveProcess(
    const std::string& host_name, pid_t process_id) {
  return impl_->RemoveProcess(host_name, process_id);
}
bool ChannelManager::HasReader(const std::string& name) const {
  return impl_->HasRole(impl_->readers, name);
}
bool ChannelManager::HasWriter(const std::string& name) const {
  return impl_->HasRole(impl_->writers, name);
}
std::vector<proto::RoleAttributes> ChannelManager::GetReaders(
    const std::string& name) const {
  return impl_->Snapshot(impl_->readers, name);
}
std::vector<proto::RoleAttributes> ChannelManager::GetWriters(
    const std::string& name) const {
  return impl_->Snapshot(impl_->writers, name);
}
void ChannelManager::AddNode(const std::string& name, pid_t process_id) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->nodes[name] = process_id;
}
bool ChannelManager::HasNode(const std::string& name, pid_t process_id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto node = impl_->nodes.find(name);
  return node != impl_->nodes.end() && node->second == process_id;
}
std::string ChannelManager::DumpGraph() const {
  Impl::RoleTable writers;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    writers = impl_->writers;
  }
  std::ostringstream stream;
  stream << "digraph minicyber_topology {\n";
  for (const auto& channel : writers) {
    const auto readers = GetReaders(channel.first);
    for (const auto& writer : channel.second) {
      for (const auto& reader : readers) {
        stream << "  \"" << writer.second.node_name() << "\" -> \""
               << reader.node_name() << "\" [label=\"" << channel.first
               << "\"];\n";
      }
    }
  }
  stream << "}\n";
  return stream.str();
}
void ChannelManager::Clear() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->writers.clear();
  impl_->readers.clear();
  impl_->nodes.clear();
}

}  // namespace service_discovery
}  // namespace minicyber
