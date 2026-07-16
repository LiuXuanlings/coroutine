#ifndef MINICYBER_TOPOLOGY_TOPOLOGY_MANAGER_H_
#define MINICYBER_TOPOLOGY_TOPOLOGY_MANAGER_H_

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "minicyber/common/types.h"

namespace minicyber {
namespace topology {

// =============================================================================
// TopologyManager：轻量级拓扑管理器（对齐 CyberRT TopologyManager 的子集）
//
// 职责：管理节点-通道的发布/订阅关系，维护一张有向图：
//   Node(Writer) --channel--> Node(Reader)
// 核心方法 IsSameProc(channel)：判断该 channel 的所有 writer 和 reader
// 是否都在同一进程内——若是，Step 23 的 Transport 可选用 INTRA 零拷贝；
// 否则必须走 SHM 跨进程。
//
// 与 CyberRT 的简化：
//   - 去掉 FastRTPS 跨主机发现、NodeManager/ChannelManager/ServiceManager
//     三个子管理器、Participant 通信、ChangeSignal
//   - 单机单进程内用 mutex + unordered_map 维护拓扑，足够覆盖 MiniCyber 场景
//   - channel 用 string 名称（业务层语义），transport 层自行 hash 成 channel_id
//
// 数据模型：
//   ChannelInfo {
//     std::vector<RoleAttr> writers;  // {node_name, pid}
//     std::vector<RoleAttr> readers;
//   }
//   unordered_map<string, ChannelInfo> channels_
//   unordered_map<string, pid_t> nodes_  // node_name -> pid（简化）
//
// IsSameProc 语义：
//   - channel 必须同时有 writer 和 reader
//   - 且所有 writer 和 reader 的 pid 相同
//   - 满足以上两点才返回 true
// =============================================================================

struct RoleAttr {
  std::string node_name;
  pid_t pid = 0;
};

struct ChannelInfo {
  std::vector<RoleAttr> writers;
  std::vector<RoleAttr> readers;
};

class TopologyManager {
 public:
  static TopologyManager* Instance() {
    static TopologyManager inst;
    return &inst;
  }
  ~TopologyManager() = default;

  // 注册一个节点（node_name + pid）
  void AddNode(const std::string& node_name, pid_t pid);

  // 在 channel 上注册一个 writer / reader（关联到 node_name + pid）
  void AddChannelWriter(const std::string& channel,
                        const std::string& node_name, pid_t pid);
  void AddChannelReader(const std::string& channel,
                        const std::string& node_name, pid_t pid);

  // 查询节点是否存在
  bool HasNode(const std::string& node_name, pid_t pid) const;

  // 核心：判断 channel 是否纯同进程通信
  // true 当且仅当 channel 有 writer 且有 reader，且所有角色 pid 相同
  bool IsSameProc(const std::string& channel) const;

  // 更细粒度：返回 channel 与给定 pid 的关系
  // SAME_PROC  : channel 纯同进程，且 pid 匹配
  // DIFF_PROC  : channel 有跨进程角色
  // NO_RELATION: channel 不存在或无完整通信路径
  Relation GetRelation(const std::string& channel, pid_t pid) const;

  // 输出有向图（用于调试/可视化）
  std::string DumpGraph() const;

  // 清空所有拓扑（测试用）
  void Shutdown();

 private:
  TopologyManager() = default;
  TopologyManager(const TopologyManager&) = delete;
  TopologyManager& operator=(const TopologyManager&) = delete;

  // 幂等添加 RoleAttr（node_name + pid 相同则不重复）
  static void AddRole(std::vector<RoleAttr>& vec,
                      const std::string& node_name, pid_t pid);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, ChannelInfo> channels_;
  std::unordered_map<std::string, pid_t> nodes_;
};

}  // namespace topology
}  // namespace minicyber

#endif  // MINICYBER_TOPOLOGY_TOPOLOGY_MANAGER_H_