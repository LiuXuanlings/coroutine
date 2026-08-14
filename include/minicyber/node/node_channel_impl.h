#ifndef MINICYBER_NODE_NODE_CHANNEL_IMPL_H_
#define MINICYBER_NODE_NODE_CHANNEL_IMPL_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/message.h>

#include <unistd.h>

#include "minicyber/node/reader.h"
#include "minicyber/node/writer.h"
#include "minicyber/transport/transport.h"

namespace minicyber {
namespace node {

// NodeChannelImpl 对齐 CyberRT 的端点属性填充职责。RoleAttributes 是发现与
// HybridTransport 的共同连接键，因此必须在创建 Reader/Writer 前完整确定；
// Node 本身不维护第二套拓扑状态。
class NodeChannelImpl {
 public:
  explicit NodeChannelImpl(std::string node_name)
      : node_name_(std::move(node_name)), process_id_(::getpid()) {
    char host_name[256] = {};
    host_name_ = ::gethostname(host_name, sizeof(host_name) - 1) == 0
                     ? host_name
                     : "unknown";
  }

  template <typename T>
  std::shared_ptr<Writer<T>> CreateWriter(const std::string& channel) const {
    static_assert(std::is_base_of<google::protobuf::Message, T>::value,
                  "Node Channel messages must derive from google::protobuf::Message");
    if (channel.empty()) return nullptr;
    auto writer = std::make_shared<Writer<T>>(MakeRole<T>(channel, "writer"));
    return writer->Init() ? writer : nullptr;
  }

  template <typename T>
  std::shared_ptr<Reader<T>> CreateReader(
      const std::string& channel,
      const typename Reader<T>::CallbackFunc& callback,
      uint32_t pending_queue_size = Reader<T>::kDefaultPendingQueueSize) const {
    static_assert(std::is_base_of<google::protobuf::Message, T>::value,
                  "Node Channel messages must derive from google::protobuf::Message");
    if (channel.empty()) return nullptr;
    auto reader = std::make_shared<Reader<T>>(
        MakeRole<T>(channel, "reader"), callback, pending_queue_size);
    return reader->Init() ? reader : nullptr;
  }

 private:
  template <typename T>
  proto::RoleAttributes MakeRole(const std::string& channel,
                                 const char* endpoint_kind) const {
    proto::RoleAttributes attr;
    attr.set_host_name(host_name_);
    // 本项目只做同机发现；保留字段以对齐原生 RoleAttributes，而不创建网络数据面。
    attr.set_host_ip("127.0.0.1");
    attr.set_process_id(process_id_);
    attr.set_node_name(node_name_);
    attr.set_node_id(std::hash<std::string>{}(node_name_));
    attr.set_channel_name(channel);
    attr.set_channel_id(transport::Transport::ChannelNameToId(channel));

    auto prototype = std::make_unique<T>();
    const auto* descriptor = prototype->GetDescriptor();
    if (descriptor != nullptr) {
      attr.set_message_type(descriptor->full_name());
      if (descriptor->file() != nullptr) {
        google::protobuf::FileDescriptorProto file_descriptor;
        descriptor->file()->CopyTo(&file_descriptor);
        std::string serialized_descriptor;
        if (file_descriptor.SerializeToString(&serialized_descriptor)) {
          attr.set_proto_desc(serialized_descriptor);
        }
      }
    }

    // 端点 id 是同一 Node 同一 Channel 的区分键；ChannelManager 用它幂等处理
    // Join/Leave，HybridTransport 用它维护每个 opposite 的连接集合。
    const auto serial = next_endpoint_id_.fetch_add(1, std::memory_order_relaxed);
    attr.set_id(std::hash<std::string>{}(
        node_name_ + channel + endpoint_kind + std::to_string(process_id_) +
        std::to_string(serial)));
    return attr;
  }

  std::string node_name_;
  std::string host_name_;
  pid_t process_id_;
  mutable std::atomic<uint64_t> next_endpoint_id_{1};
};

}  // namespace node
}  // namespace minicyber

#endif  // MINICYBER_NODE_NODE_CHANNEL_IMPL_H_
