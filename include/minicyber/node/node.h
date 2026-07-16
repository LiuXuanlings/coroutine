#ifndef MINICYBER_NODE_NODE_H_
#define MINICYBER_NODE_NODE_H_

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "minicyber/node/reader.h"
#include "minicyber/node/writer.h"
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
//   - 去掉 NodeChannelImpl / NodeServiceImpl / Service / Client
//   - 去掉 CreateTask / Observe / ClearData（Reader 不再创建协程）
//   - 拓扑注册在 Writer/Reader::Init 时完成，Node 只负责注册自身节点名
// =============================================================================

// ReaderBase 前置声明（用于 map 存储异构 Reader）
class ReaderBaseHolder {
 public:
  virtual ~ReaderBaseHolder() = default;
};

template <typename T>
class ReaderHolder : public ReaderBaseHolder {
 public:
  std::shared_ptr<Reader<T>> reader;
  explicit ReaderHolder(std::shared_ptr<Reader<T>> r) : reader(std::move(r)) {}
};

class Node {
 public:
  explicit Node(const std::string& name) : node_name_(name) {
    topology::TopologyManager::Instance()->AddNode(node_name_, ::getpid());
  }

  ~Node() {
    std::lock_guard<std::mutex> lg(readers_mutex_);
    for (auto& kv : readers_) {
      kv.second.reset();  // 析构 ReaderBaseHolder -> Reader -> Shutdown
    }
    readers_.clear();
    // Writer 由 shared_ptr 析构时自动 Shutdown
    writers_.clear();
  }

  const std::string& Name() const { return node_name_; }

  // 创建 Writer
  template <typename T>
  std::shared_ptr<Writer<T>> CreateWriter(const std::string& channel) {
    auto w = std::make_shared<Writer<T>>(node_name_, channel);
    w->Init();
    std::lock_guard<std::mutex> lg(readers_mutex_);
    writers_.push_back(std::shared_ptr<void>(
        static_cast<void*>(nullptr), [w](void*) { (void)w; }));
    return w;
  }

  // 创建 Reader
  template <typename T>
  std::shared_ptr<Reader<T>> CreateReader(
      const std::string& channel,
      const typename Reader<T>::CallbackFunc& callback = nullptr) {
    std::lock_guard<std::mutex> lg(readers_mutex_);
    if (readers_.find(channel) != readers_.end()) {
      return nullptr;  // 同 channel 不重复创建
    }
    auto r = std::make_shared<Reader<T>>(node_name_, channel, callback);
    r->Init();
    auto holder = std::make_shared<ReaderHolder<T>>(r);
    readers_[channel] = holder;
    return r;
  }

  // 获取已创建的 Reader
  template <typename T>
  std::shared_ptr<Reader<T>> GetReader(const std::string& channel) {
    std::lock_guard<std::mutex> lg(readers_mutex_);
    auto it = readers_.find(channel);
    if (it == readers_.end()) return nullptr;
    auto holder = std::dynamic_pointer_cast<ReaderHolder<T>>(it->second);
    if (holder == nullptr) return nullptr;
    return holder->reader;
  }

 private:
  std::string node_name_;
  std::mutex readers_mutex_;
  std::map<std::string, std::shared_ptr<ReaderBaseHolder>> readers_;
  std::vector<std::shared_ptr<void>> writers_;
};

}  // namespace node
}  // namespace minicyber

#endif  // MINICYBER_NODE_NODE_H_