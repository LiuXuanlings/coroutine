#ifndef MINICYBER_NODE_READER_H_
#define MINICYBER_NODE_READER_H_

#include <functional>
#include <memory>
#include <string>

#include "minicyber/topology/topology_manager.h"
#include "minicyber/transport/receiver/receiver.h"
#include "minicyber/transport/transport.h"

namespace minicyber {
namespace node {

// =============================================================================
// Reader：订阅端用户接口（对齐 CyberRT Reader<M>）
//
// 职责：封装 Receiver，提供回调式消息接收 API。
//   创建时自动向 TopologyManager 注册为 reader，使 Transport 路由可感知拓扑。
//
// 生命周期：
//   Init()    : 注册拓扑 + Transport::CreateReceiver<T>
//   Shutdown(): receiver_->Disable()
//
// 回调机制：
//   用户传入 CallbackFunc，Reader 内部传给 Transport::CreateReceiver，
//   由底层 Receiver 在数据到达时调用。
//
// 与 CyberRT 的简化：
//   - 去掉 RoleAttributes / ReaderBase / Blocker / DataVisitor / 协程任务创建
//   - 去掉 HasWriter / GetWriters / 动态拓扑监听
//   - 去掉 Observe / ClearData / GetDelaySec 等阻塞器接口
//   - 拓扑在 Init 时静态注册，不做动态变更通知
// =============================================================================

template <typename T>
class Reader {
 public:
  using CallbackFunc = std::function<void(const std::shared_ptr<T>&)>;

  Reader(const std::string& node_name, const std::string& channel,
         const CallbackFunc& callback = nullptr)
      : node_name_(node_name), channel_(channel), callback_(callback) {}

  ~Reader() { Shutdown(); }

  // 注册拓扑 + 创建底层 Receiver
  void Init() {
    if (init_) return;
    topology::TopologyManager::Instance()->AddChannelReader(
        channel_, node_name_, ::getpid());
    receiver_ = transport::Transport::CreateReceiver<T>(channel_, callback_);
    init_ = true;
  }

  void Shutdown() {
    if (!init_) return;
    init_ = false;
    if (receiver_) {
      receiver_->Disable();
      receiver_.reset();
    }
  }

  bool IsInit() const { return init_; }
  const std::string& channel() const { return channel_; }

 private:
  std::string node_name_;
  std::string channel_;
  CallbackFunc callback_;
  std::shared_ptr<transport::Receiver<T>> receiver_;
  bool init_ = false;
};

}  // namespace node
}  // namespace minicyber

#endif  // MINICYBER_NODE_READER_H_