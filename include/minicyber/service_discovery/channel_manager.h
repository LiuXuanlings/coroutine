#ifndef MINICYBER_SERVICE_DISCOVERY_CHANNEL_MANAGER_H_
#define MINICYBER_SERVICE_DISCOVERY_CHANNEL_MANAGER_H_

#include <memory>
#include <string>
#include <vector>

#include <sys/types.h>

#include "minicyber/proto/role_attributes.pb.h"
#include "minicyber/proto/topology_change.pb.h"

namespace minicyber {
namespace service_discovery {

// ChannelManager 是 Channel 角色状态的唯一所有者。TopologyManager 只负责
// FastRTPS 控制消息和 ChangeSignal 编排，避免形成第二套本地拓扑表。
class ChannelManager {
 public:
  // Participant 异常离场时，TopologyManager 用返回快照补发与显式 Leave
  // 相同的通知。
  struct RemovedRole {
    proto::RoleType role_type;
    proto::RoleAttributes attr;
  };

  ChannelManager();
  ~ChannelManager();

  // 幂等应用已经过 TopologyManager 校验的 Channel 变更；仅在状态真实变化时
  // 返回 true。
  bool Apply(const proto::ChangeMsg& msg);
  // 原子删除指定进程的全部角色，避免查询方看到只清理一半的进程快照。
  std::vector<RemovedRole> RemoveProcess(const std::string& host_name,
                                         pid_t process_id);
  bool HasReader(const std::string& channel_name) const;
  bool HasWriter(const std::string& channel_name) const;
  std::vector<proto::RoleAttributes> GetReaders(
      const std::string& channel_name) const;
  std::vector<proto::RoleAttributes> GetWriters(
      const std::string& channel_name) const;

  void AddNode(const std::string& node_name, pid_t process_id);
  bool HasNode(const std::string& node_name, pid_t process_id) const;
  std::string DumpGraph() const;
  void Clear();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace service_discovery
}  // namespace minicyber

#endif  // MINICYBER_SERVICE_DISCOVERY_CHANNEL_MANAGER_H_
