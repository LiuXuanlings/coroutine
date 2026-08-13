#ifndef MINICYBER_NODE_NODE_CHANNEL_IMPL_H_
#define MINICYBER_NODE_NODE_CHANNEL_IMPL_H_

#include <memory>
#include <string>

#include "minicyber/node/reader.h"
#include "minicyber/node/writer.h"

namespace minicyber {
namespace node {

// Node 的通道构建边界。MiniCyber 没有 RoleAttributes/QoS 配置对象，保留
// 原生 NodeChannelImpl 的职责：校验 channel 并初始化 Reader/Writer。
class NodeChannelImpl {
 public:
  explicit NodeChannelImpl(std::string node_name)
      : node_name_(std::move(node_name)) {}

  template <typename T>
  std::shared_ptr<Writer<T>> CreateWriter(const std::string& channel) const {
    if (channel.empty()) return nullptr;
    auto writer = std::make_shared<Writer<T>>(node_name_, channel);
    writer->Init();
    return writer->IsInit() ? writer : nullptr;
  }

  template <typename T>
  std::shared_ptr<Reader<T>> CreateReader(
      const std::string& channel,
      const typename Reader<T>::CallbackFunc& callback) const {
    if (channel.empty()) return nullptr;
    auto reader = std::make_shared<Reader<T>>(node_name_, channel, callback);
    reader->Init();
    return reader->IsInit() ? reader : nullptr;
  }

 private:
  std::string node_name_;
};

}  // namespace node
}  // namespace minicyber

#endif  // MINICYBER_NODE_NODE_CHANNEL_IMPL_H_
