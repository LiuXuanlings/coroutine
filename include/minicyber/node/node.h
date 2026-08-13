#ifndef MINICYBER_NODE_NODE_H_
#define MINICYBER_NODE_NODE_H_

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

#include "minicyber/node/node_channel_impl.h"
#include "minicyber/service/client.h"
#include "minicyber/service/service.h"
#include "minicyber/topology/topology_manager.h"

namespace minicyber {
namespace node {

// =============================================================================
// Node：顶层用户 API 入口（对齐 CyberRT Node）
//
// 职责：
//   1. 作为 Reader/Writer 的工厂
//   2. 管理所创建 Reader 的生命周期
//   3. 构造时自动向 TopologyManager 注册节点
//
// 用法：
//   Node node("talker");
//   auto w = node.CreateWriter<std::string>("/chatter");
//   auto r = node.CreateReader<std::string>("/chatter", [](msg) { ... });
//
// 与 CyberRT 的简化：
//   - 保留精简 NodeChannelImpl；去掉 RoleAttributes、QoS 和动态拓扑监听
//   - 去掉 CreateTask / Observe / ClearData（Reader 不再创建协程）
//   - 拓扑注册在 Writer/Reader::Init 时完成，Node 只负责注册自身节点名
// =============================================================================

// ReaderBase 前置声明（用于 map 存储异构 Reader）
class ManagedEndpoint {
 public:
  virtual ~ManagedEndpoint() = default;
  virtual void Shutdown() = 0;
};

template <typename T>
class ReaderHolder : public ManagedEndpoint {
 public:
  std::shared_ptr<Reader<T>> reader;
  explicit ReaderHolder(std::shared_ptr<Reader<T>> r) : reader(std::move(r)) {}
  void Shutdown() override { reader->Shutdown(); }
};

template <typename T>
class WriterHolder : public ManagedEndpoint {
 public:
  explicit WriterHolder(std::shared_ptr<Writer<T>> writer)
      : writer_(std::move(writer)) {}
  void Shutdown() override { writer_->Shutdown(); }

 private:
  std::shared_ptr<Writer<T>> writer_;
};

template <typename T>
class ServiceHolder : public ManagedEndpoint {
 public:
  explicit ServiceHolder(std::shared_ptr<T> service) : service_(std::move(service)) {}
  void Shutdown() override { service_->Destroy(); }

 private:
  std::shared_ptr<T> service_;
};

class Node {
 public:
  explicit Node(const std::string& name) : node_name_(name) {
    topology::TopologyManager::Instance()->AddNode(node_name_, ::getpid());
    channel_impl_ = std::make_unique<NodeChannelImpl>(node_name_);
  }

  ~Node() { Shutdown(); }

  const std::string& Name() const { return node_name_; }
  bool IsShutdown() const {
    std::lock_guard<std::mutex> lg(endpoints_mutex_);
    return shutdown_;
  }

  // 关闭 Node 创建的所有 endpoint。端点可被调用方继续持有，但不能再通信。
  void Shutdown() {
    std::vector<std::shared_ptr<ManagedEndpoint>> endpoints;
    {
      std::lock_guard<std::mutex> lg(endpoints_mutex_);
      if (shutdown_) return;
      shutdown_ = true;
      for (const auto& entry : readers_) endpoints.push_back(entry.second);
      endpoints.insert(endpoints.end(), writers_.begin(), writers_.end());
      endpoints.insert(endpoints.end(), services_.begin(), services_.end());
      endpoints.insert(endpoints.end(), clients_.begin(), clients_.end());
      readers_.clear();
      writers_.clear();
      services_.clear();
      clients_.clear();
    }
    for (const auto& endpoint : endpoints) endpoint->Shutdown();
  }

  // 创建 Writer
  template <typename T>
  std::shared_ptr<Writer<T>> CreateWriter(const std::string& channel) {
    std::lock_guard<std::mutex> lg(endpoints_mutex_);
    if (shutdown_) return nullptr;
    auto w = channel_impl_->template CreateWriter<T>(channel);
    if (w == nullptr) return nullptr;
    writers_.push_back(std::make_shared<WriterHolder<T>>(w));
    return w;
  }

  // 创建 Reader
  template <typename T>
  std::shared_ptr<Reader<T>> CreateReader(
      const std::string& channel,
      const typename Reader<T>::CallbackFunc& callback = nullptr) {
    std::lock_guard<std::mutex> lg(endpoints_mutex_);
    if (shutdown_) return nullptr;
    if (readers_.find(channel) != readers_.end()) {
      return nullptr;  // 同 channel 不重复创建
    }
    auto r = channel_impl_->template CreateReader<T>(channel, callback);
    if (r == nullptr) return nullptr;
    auto holder = std::make_shared<ReaderHolder<T>>(r);
    readers_[channel] = holder;
    return r;
  }

  // 获取已创建的 Reader
  template <typename T>
  std::shared_ptr<Reader<T>> GetReader(const std::string& channel) {
    std::lock_guard<std::mutex> lg(endpoints_mutex_);
    auto it = readers_.find(channel);
    if (it == readers_.end()) return nullptr;
    auto holder = std::dynamic_pointer_cast<ReaderHolder<T>>(it->second);
    if (holder == nullptr) return nullptr;
    return holder->reader;
  }

  // 创建 Service（RPC 服务端）
  // 自动注册拓扑 + Init。Service 生命周期由 Node 持有。
  template <typename Req, typename Rsp>
  std::shared_ptr<service::Service<Req, Rsp>> CreateService(
      const std::string& service_name,
      const typename service::Service<Req, Rsp>::ServiceCallback& callback) {
    auto s = std::make_shared<service::Service<Req, Rsp>>(node_name_,
                                                           service_name, callback);
    if (!s->Init()) return nullptr;
    std::lock_guard<std::mutex> lg(endpoints_mutex_);
    if (shutdown_) {
      s->Destroy();
      return nullptr;
    }
    services_.push_back(std::make_shared<ServiceHolder<service::Service<Req, Rsp>>>(s));
    return s;
  }

  // 创建 Client（RPC 客户端）
  // 自动注册拓扑 + Init。Client 生命周期由 Node 持有。
  template <typename Req, typename Rsp>
  std::shared_ptr<service::Client<Req, Rsp>> CreateClient(
      const std::string& service_name) {
    auto c = std::make_shared<service::Client<Req, Rsp>>(node_name_, service_name);
    if (!c->Init()) return nullptr;
    std::lock_guard<std::mutex> lg(endpoints_mutex_);
    if (shutdown_) {
      c->Destroy();
      return nullptr;
    }
    clients_.push_back(std::make_shared<ServiceHolder<service::Client<Req, Rsp>>>(c));
    return c;
  }

 private:
  std::string node_name_;
  std::unique_ptr<NodeChannelImpl> channel_impl_;
  mutable std::mutex endpoints_mutex_;
  std::map<std::string, std::shared_ptr<ManagedEndpoint>> readers_;
  std::vector<std::shared_ptr<ManagedEndpoint>> writers_;
  std::vector<std::shared_ptr<ManagedEndpoint>> services_;
  std::vector<std::shared_ptr<ManagedEndpoint>> clients_;
  bool shutdown_ = false;
};

}  // namespace node
}  // namespace minicyber

#endif  // MINICYBER_NODE_NODE_H_
