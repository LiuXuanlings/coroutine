#ifndef MINICYBER_TOPOLOGY_TOPOLOGY_MANAGER_H_
#define MINICYBER_TOPOLOGY_TOPOLOGY_MANAGER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <sys/types.h>

#include "minicyber/common/types.h"
#include "minicyber/proto/role_attributes.pb.h"
#include "minicyber/proto/topology_change.pb.h"

namespace minicyber {
namespace topology {

class TopologyManager {
 public:
  using ChangeFunc = std::function<void(const proto::ChangeMsg&)>;

  struct ChangeConnection {
    uint64_t id = 0;
  };

  static TopologyManager* Instance();
  ~TopologyManager();

  bool Start();
  void Shutdown();

  bool Join(proto::RoleType role_type, const proto::RoleAttributes& attr);
  bool Leave(proto::RoleType role_type, const proto::RoleAttributes& attr);
  bool HasReader(const std::string& channel_name) const;
  bool HasWriter(const std::string& channel_name) const;
  std::vector<proto::RoleAttributes> GetReaders(
      const std::string& channel_name) const;
  std::vector<proto::RoleAttributes> GetWriters(
      const std::string& channel_name) const;
  Relation GetRelation(const std::string& channel_name, pid_t process_id) const;
  bool IsSameProc(const std::string& channel_name) const;

  ChangeConnection AddChangeListener(ChangeFunc callback);
  void RemoveChangeListener(ChangeConnection connection);

  // 这些兼容入口仍写入同一 Channel 状态表，供 MC-608 前的 Node/测试使用。
  void AddNode(const std::string& node_name, pid_t process_id);
  void AddChannelWriter(const std::string& channel_name,
                        const std::string& node_name, pid_t process_id);
  void AddChannelReader(const std::string& channel_name,
                        const std::string& node_name, pid_t process_id);
  bool HasNode(const std::string& node_name, pid_t process_id) const;
  std::string DumpGraph() const;

 private:
  TopologyManager();
  TopologyManager(const TopologyManager&) = delete;
  TopologyManager& operator=(const TopologyManager&) = delete;

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace topology
}  // namespace minicyber

#endif  // MINICYBER_TOPOLOGY_TOPOLOGY_MANAGER_H_
