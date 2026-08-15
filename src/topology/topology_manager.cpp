#include "minicyber/topology/topology_manager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>

#include <fastdds/rtps/common/SerializedPayload.h>
#include <fastrtps/Domain.h>
#include <fastrtps/TopicDataType.h>
#include <fastrtps/attributes/ParticipantAttributes.h>
#include <fastrtps/attributes/PublisherAttributes.h>
#include <fastrtps/attributes/SubscriberAttributes.h>
#include <fastrtps/qos/QosPolicies.h>
#include <fastrtps/participant/ParticipantListener.h>
#include <fastrtps/publisher/Publisher.h>
#include <fastrtps/subscriber/SampleInfo.h>
#include <fastrtps/subscriber/Subscriber.h>
#include <fastrtps/subscriber/SubscriberListener.h>
#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>

#include "minicyber/service_discovery/channel_manager.h"
#include "minicyber/time/time.h"

namespace minicyber {
namespace topology {
namespace {

constexpr char kControlTopic[] = "minicyber_channel_change";
constexpr char kControlType[] = "minicyber.proto.ChangeMsg";
constexpr char kParticipantNamePrefix[] = "minicyber:";
constexpr uint32_t kControlPayloadSize = 65536;
constexpr uint32_t kCdrEncapsulationSize = 4;
constexpr uint32_t kCdrSequenceLengthSize = 4;

std::string LocalHostName() {
  char name[256] = {};
  return gethostname(name, sizeof(name) - 1) == 0 ? name : "unknown";
}

bool ParseParticipantName(const std::string& name, std::string* host_name,
                          pid_t* process_id) {
  if (name.rfind(kParticipantNamePrefix, 0) != 0) {
    return false;
  }
  const auto separator = name.rfind('+');
  if (separator == std::string::npos ||
      separator <= std::strlen(kParticipantNamePrefix)) {
    return false;
  }
  try {
    const long parsed_pid = std::stol(name.substr(separator + 1));
    if (parsed_pid <= 0 || parsed_pid > std::numeric_limits<pid_t>::max()) {
      return false;
    }
    *host_name = name.substr(std::strlen(kParticipantNamePrefix),
                             separator - std::strlen(kParticipantNamePrefix));
    *process_id = static_cast<pid_t>(parsed_pid);
    return !host_name->empty();
  } catch (const std::exception&) {
    return false;
  }
}

bool IsChannelRole(proto::RoleType role_type) {
  return role_type == proto::ROLE_WRITER || role_type == proto::ROLE_READER;
}

bool IsChannelOperation(proto::OperateType operate_type) {
  return operate_type == proto::OPT_JOIN || operate_type == proto::OPT_LEAVE;
}

bool ShouldInjectStartupFailure(const char* step) {
#if defined(MINICYBER_ENABLE_TEST_FAILURES)
  // 故障门禁只在 Debug 核心库中编译，不扩张 TopologyManager 公开 API，也不改变
  // Release 控制面。测试用它稳定覆盖 FastRTPS 分步创建后的逆序回滚。
  const char* requested = std::getenv("MINICYBER_TEST_TOPOLOGY_START_FAIL");
  return requested != nullptr && std::strcmp(requested, step) == 0;
#else
  static_cast<void>(step);
  return false;
#endif
}

class ChangeMsgType final : public eprosima::fastrtps::TopicDataType {
 public:
  ChangeMsgType() {
    setName(kControlType);
    m_typeSize = kControlPayloadSize;
    m_isGetKeyDefined = false;
  }

  bool serialize(void* data,
                 eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
    const auto& msg = *static_cast<proto::ChangeMsg*>(data);
    const size_t size = msg.ByteSizeLong();
    if (size + kCdrEncapsulationSize + kCdrSequenceLengthSize >
            payload->max_size ||
        size + kCdrEncapsulationSize + kCdrSequenceLengthSize >
            kControlPayloadSize) {
      return false;
    }
    eprosima::fastcdr::FastBuffer buffer(
        reinterpret_cast<char*>(payload->data), payload->max_size);
    eprosima::fastcdr::Cdr serializer(
        buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
        eprosima::fastcdr::Cdr::DDS_CDR);
    serializer.serialize_encapsulation();
    std::vector<uint8_t> bytes(size);
    if (!msg.SerializeToArray(bytes.data(), static_cast<int>(size))) {
      return false;
    }
    serializer << bytes;
    payload->encapsulation = serializer.endianness() ==
                                     eprosima::fastcdr::Cdr::BIG_ENDIANNESS
                                 ? CDR_BE
                                 : CDR_LE;
    payload->length = static_cast<uint32_t>(serializer.getSerializedDataLength());
    return true;
  }

  bool deserialize(eprosima::fastrtps::rtps::SerializedPayload_t* payload,
                   void* data) override {
    if (payload->length < kCdrEncapsulationSize) {
      return false;
    }
    try {
      eprosima::fastcdr::FastBuffer buffer(
          reinterpret_cast<char*>(payload->data), payload->length);
      eprosima::fastcdr::Cdr deserializer(
          buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
          eprosima::fastcdr::Cdr::DDS_CDR);
      deserializer.read_encapsulation();
      std::vector<uint8_t> bytes;
      deserializer >> bytes;
      return static_cast<proto::ChangeMsg*>(data)->ParseFromArray(
          bytes.data(), static_cast<int>(bytes.size()));
    } catch (const eprosima::fastcdr::exception::Exception&) {
      return false;
    }
  }

  std::function<uint32_t()> getSerializedSizeProvider(void* data) override {
    const auto size = static_cast<proto::ChangeMsg*>(data)->ByteSizeLong();
    return [size] {
      return static_cast<uint32_t>(size + kCdrEncapsulationSize +
                                   kCdrSequenceLengthSize);
    };
  }

  void* createData() override { return new proto::ChangeMsg(); }
  void deleteData(void* data) override { delete static_cast<proto::ChangeMsg*>(data); }
  bool getKey(void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override {
    return false;
  }
};

class ControlParticipant {
 public:
  using Callback = std::function<void(const proto::ChangeMsg&)>;
  using ParticipantLeaveCallback = std::function<void(const std::string&, pid_t)>;

  ControlParticipant(Callback callback, ParticipantLeaveCallback participant_leave_callback)
      : callback_(std::move(callback)),
        participant_listener_(std::move(participant_leave_callback)),
        subscriber_listener_([this](const proto::ChangeMsg& msg) {
          // 本地状态已在 Publish 前提交；过滤 DDS 自回环，确保监听器只收到
          // 一次事件。
          if (!IsSelfMessage(msg)) {
            callback_(msg);
          }
        }) {}
  ~ControlParticipant() { Shutdown(); }

  bool Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (participant_ != nullptr) {
      return true;
    }

    try {
      if (ShouldInjectStartupFailure("participant")) {
        return false;
      }
      eprosima::fastrtps::ParticipantAttributes attributes;
      eprosima::fastrtps::Domain::getDefaultParticipantAttributes(attributes);
      attributes.domainId = 80;
      attributes.rtps.setName((std::string(kParticipantNamePrefix) + LocalHostName() + "+" +
                               std::to_string(getpid())).c_str());
      participant_ = eprosima::fastrtps::Domain::createParticipant(
          attributes, &participant_listener_);
      if (participant_ == nullptr) {
        return false;
      }

      if (ShouldInjectStartupFailure("type")) {
        ShutdownLocked();
        return false;
      }
      type_ = std::make_unique<ChangeMsgType>();
      if (!eprosima::fastrtps::Domain::registerType(participant_, type_.get())) {
        ShutdownLocked();
        return false;
      }

      if (ShouldInjectStartupFailure("publisher")) {
        ShutdownLocked();
        return false;
      }
      // 原生拓扑 QoS 使用可靠传输和本地持久化，保证晚加入进程能重放
      // 端点状态。
      eprosima::fastrtps::PublisherAttributes publisher_attributes;
      eprosima::fastrtps::Domain::getDefaultPublisherAttributes(publisher_attributes);
      publisher_attributes.qos.m_reliability.kind =
          eprosima::fastrtps::RELIABLE_RELIABILITY_QOS;
      publisher_attributes.qos.m_durability.kind =
          eprosima::fastrtps::TRANSIENT_LOCAL_DURABILITY_QOS;
      publisher_attributes.topic.historyQos.kind = eprosima::fastrtps::KEEP_ALL_HISTORY_QOS;
      publisher_attributes.topic.historyQos.depth = 10;
      publisher_attributes.topic.topicName = kControlTopic;
      publisher_attributes.topic.topicDataType = kControlType;
      publisher_ = eprosima::fastrtps::Domain::createPublisher(participant_, publisher_attributes);

      if (ShouldInjectStartupFailure("subscriber")) {
        ShutdownLocked();
        return false;
      }
      eprosima::fastrtps::SubscriberAttributes subscriber_attributes;
      eprosima::fastrtps::Domain::getDefaultSubscriberAttributes(subscriber_attributes);
      subscriber_attributes.qos.m_reliability.kind =
          eprosima::fastrtps::RELIABLE_RELIABILITY_QOS;
      subscriber_attributes.qos.m_durability.kind =
          eprosima::fastrtps::TRANSIENT_LOCAL_DURABILITY_QOS;
      subscriber_attributes.topic.historyQos.kind = eprosima::fastrtps::KEEP_ALL_HISTORY_QOS;
      subscriber_attributes.topic.historyQos.depth = 10;
      subscriber_attributes.topic.topicName = kControlTopic;
      subscriber_attributes.topic.topicDataType = kControlType;
      subscriber_ = eprosima::fastrtps::Domain::createSubscriber(
          participant_, subscriber_attributes, &subscriber_listener_);
      if (publisher_ == nullptr || subscriber_ == nullptr) {
        ShutdownLocked();
        return false;
      }
    } catch (const std::exception& error) {
      std::cerr << "Topology control participant startup failed: " << error.what()
                << std::endl;
      ShutdownLocked();
      return false;
    }
    return true;
  }

  bool Publish(const proto::ChangeMsg& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    return publisher_ != nullptr && publisher_->write(const_cast<proto::ChangeMsg*>(&msg));
  }

  void Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    ShutdownLocked();
  }

 private:
  class ParticipantListener final : public eprosima::fastrtps::ParticipantListener {
   public:
    explicit ParticipantListener(ParticipantLeaveCallback participant_leave_callback)
        : participant_leave_callback_(std::move(participant_leave_callback)) {}

    void onParticipantDiscovery(
        eprosima::fastrtps::Participant*,
        eprosima::fastrtps::rtps::ParticipantDiscoveryInfo&& info) override {
      if (info.status !=
              eprosima::fastrtps::rtps::ParticipantDiscoveryInfo::REMOVED_PARTICIPANT &&
          info.status !=
              eprosima::fastrtps::rtps::ParticipantDiscoveryInfo::DROPPED_PARTICIPANT) {
        return;
      }
      std::string host_name;
      pid_t process_id = 0;
      if (ParseParticipantName(info.info.m_participantName.to_string(), &host_name,
                               &process_id)) {
        participant_leave_callback_(host_name, process_id);
      }
    }

   private:
    ParticipantLeaveCallback participant_leave_callback_;
  };

  class SubscriberListener final : public eprosima::fastrtps::SubscriberListener {
   public:
    explicit SubscriberListener(Callback callback) : callback_(std::move(callback)) {}

    void onNewDataMessage(eprosima::fastrtps::Subscriber* subscriber) override {
      proto::ChangeMsg msg;
      eprosima::fastrtps::SampleInfo_t info;
      while (subscriber->takeNextData(&msg, &info)) {
        if (info.sampleKind == eprosima::fastrtps::rtps::ALIVE) {
          callback_(msg);
        }
      }
    }

   private:
    Callback callback_;
  };

  void ShutdownLocked() {
    // Participant 拥有 Publisher/Subscriber；按 FastRTPS 所有权一次移除，
    // 随后清空借用指针。
    if (participant_ != nullptr) {
      eprosima::fastrtps::Domain::removeParticipant(participant_);
      participant_ = nullptr;
    }
    publisher_ = nullptr;
    subscriber_ = nullptr;
    type_.reset();
  }

  bool IsSelfMessage(const proto::ChangeMsg& msg) const {
    return msg.has_role_attr() && msg.role_attr().has_process_id() &&
           msg.role_attr().process_id() == getpid() &&
           msg.role_attr().has_host_name() &&
           msg.role_attr().host_name() == LocalHostName();
  }

  Callback callback_;
  ParticipantListener participant_listener_;
  SubscriberListener subscriber_listener_;
  std::mutex mutex_;
  eprosima::fastrtps::Participant* participant_ = nullptr;
  eprosima::fastrtps::Publisher* publisher_ = nullptr;
  eprosima::fastrtps::Subscriber* subscriber_ = nullptr;
  std::unique_ptr<ChangeMsgType> type_;
};

}  // namespace

class TopologyManager::Impl {
 public:
  struct Listener {
    explicit Listener(TopologyManager::ChangeFunc change_callback)
        : callback(std::move(change_callback)) {}

    TopologyManager::ChangeFunc callback;
    std::mutex mutex;
    std::condition_variable idle;
    bool active = true;
    size_t in_flight = 0;
  };

  Impl()
      : host_name(LocalHostName()),
        participant(
            [this](const proto::ChangeMsg& msg) { OnChange(msg); },
            [this](const std::string& host, pid_t process_id) {
              OnParticipantLeave(host, process_id);
            }) {}

  bool Start() { return participant.Start(); }

  void Shutdown() {
    participant.Shutdown();
    std::vector<std::shared_ptr<Listener>> listeners_to_remove;
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (const auto& entry : listeners) {
        listeners_to_remove.push_back(entry.second);
      }
      listeners.clear();
    }
    for (const auto& listener : listeners_to_remove) {
      DeactivateListener(listener);
    }
    channel_manager.Clear();
  }

  bool Join(proto::RoleType role_type, const proto::RoleAttributes& attr) {
    if (!IsChannelRole(role_type) || !Start() || !IsLocal(attr)) {
      return false;
    }
    proto::ChangeMsg msg = MakeChange(proto::OPT_JOIN, role_type, attr);
    OnChange(msg);
    return participant.Publish(msg);
  }

  bool Leave(proto::RoleType role_type, const proto::RoleAttributes& attr) {
    if (!IsChannelRole(role_type) || !IsLocal(attr)) {
      return false;
    }
    proto::ChangeMsg msg = MakeChange(proto::OPT_LEAVE, role_type, attr);
    OnChange(msg);
    return participant.Publish(msg);
  }

  void OnChange(const proto::ChangeMsg& msg) {
    if (!msg.has_role_attr() || !msg.has_change_type() ||
        !msg.has_operate_type() || !msg.has_role_type() ||
        msg.change_type() != proto::CHANGE_CHANNEL ||
        !IsChannelOperation(msg.operate_type()) ||
        !IsChannelRole(msg.role_type()) || !msg.role_attr().has_channel_name() ||
        msg.role_attr().channel_name().empty() ||
        !msg.role_attr().has_process_id() || !IsLocal(msg.role_attr())) {
      return;
    }

    if (channel_manager.Apply(msg)) {
      Notify(msg);
    }
  }

  void OnParticipantLeave(const std::string& host, pid_t process_id) {
    for (const auto& role : channel_manager.RemoveProcess(host, process_id)) {
      Notify(MakeChange(proto::OPT_LEAVE, role.role_type, role.attr));
    }
  }

  static bool IsExecuting(const Listener* listener) {
    return std::find(executing_listeners.begin(), executing_listeners.end(), listener) !=
           executing_listeners.end();
  }

  std::vector<std::shared_ptr<Listener>> CaptureListeners() {
    std::vector<std::shared_ptr<Listener>> result;
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& entry : listeners) {
      const auto& listener = entry.second;
      std::lock_guard<std::mutex> listener_lock(listener->mutex);
      if (listener->active) {
        ++listener->in_flight;
        result.push_back(listener);
      }
    }
    return result;
  }

  void Notify(const proto::ChangeMsg& msg) {
    // CaptureListeners 先增加 in-flight，RemoveChangeListener 才能等待
    // 已捕获回调安全退出。
    for (const auto& listener : CaptureListeners()) {
      TopologyManager::ChangeFunc callback;
      {
        std::lock_guard<std::mutex> lock(listener->mutex);
        callback = listener->callback;
      }
      executing_listeners.push_back(listener.get());
      try {
        callback(msg);
      } catch (const std::exception& error) {
        std::cerr << "Topology change listener failed: " << error.what() << std::endl;
      } catch (...) {
        std::cerr << "Topology change listener failed with an unknown exception"
                  << std::endl;
      }
      executing_listeners.pop_back();
      {
        // 即使业务监听器抛异常，也必须归还 in-flight，否则注销会永久等待。
        std::lock_guard<std::mutex> lock(listener->mutex);
        --listener->in_flight;
        if (listener->in_flight == 0) {
          listener->idle.notify_all();
        }
      }
    }
  }

  static void DeactivateListener(const std::shared_ptr<Listener>& listener) {
    std::unique_lock<std::mutex> lock(listener->mutex);
    listener->active = false;
    if (!IsExecuting(listener.get())) {
      listener->idle.wait(lock, [&listener] { return listener->in_flight == 0; });
    }
  }

  proto::ChangeMsg MakeChange(proto::OperateType operation,
                              proto::RoleType role_type,
                              const proto::RoleAttributes& attr) const {
    proto::ChangeMsg msg;
    msg.set_timestamp(time::Time::MonoTime().ToNanosecond());
    msg.set_change_type(proto::CHANGE_CHANNEL);
    msg.set_operate_type(operation);
    msg.set_role_type(role_type);
    *msg.mutable_role_attr() = attr;
    return msg;
  }

  bool IsLocal(const proto::RoleAttributes& attr) const {
    return attr.has_host_name() && attr.host_name() == host_name;
  }

  proto::RoleAttributes MakeCompatRole(const std::string& channel_name,
                                       const std::string& node_name,
                                       pid_t process_id) const {
    proto::RoleAttributes attr;
    attr.set_host_name(host_name);
    attr.set_process_id(process_id);
    attr.set_node_name(node_name);
    attr.set_channel_name(channel_name);
    attr.set_channel_id(std::hash<std::string>{}(channel_name));
    attr.set_id(std::hash<std::string>{}(node_name + channel_name) ^
                static_cast<uint64_t>(process_id));
    return attr;
  }

  const std::string host_name;
  mutable std::mutex mutex;
  std::unordered_map<uint64_t, std::shared_ptr<Listener>> listeners;
  std::atomic<uint64_t> next_listener_id{1};
  ControlParticipant participant;
  service_discovery::ChannelManager channel_manager;

 private:
  static thread_local std::vector<const Listener*> executing_listeners;
};

thread_local std::vector<const TopologyManager::Impl::Listener*>
    TopologyManager::Impl::executing_listeners;

TopologyManager* TopologyManager::Instance() {
  static TopologyManager manager;
  return &manager;
}

TopologyManager::TopologyManager() : impl_(std::make_unique<Impl>()) {}
TopologyManager::~TopologyManager() { Shutdown(); }
bool TopologyManager::Start() { return impl_->Start(); }
void TopologyManager::Shutdown() { impl_->Shutdown(); }
bool TopologyManager::Join(proto::RoleType type, const proto::RoleAttributes& attr) {
  return impl_->Join(type, attr);
}
bool TopologyManager::Leave(proto::RoleType type, const proto::RoleAttributes& attr) {
  return impl_->Leave(type, attr);
}
bool TopologyManager::HasReader(const std::string& channel_name) const {
  return impl_->channel_manager.HasReader(channel_name);
}
bool TopologyManager::HasWriter(const std::string& channel_name) const {
  return impl_->channel_manager.HasWriter(channel_name);
}
std::vector<proto::RoleAttributes> TopologyManager::GetReaders(const std::string& name) const {
  return impl_->channel_manager.GetReaders(name);
}
std::vector<proto::RoleAttributes> TopologyManager::GetWriters(const std::string& name) const {
  return impl_->channel_manager.GetWriters(name);
}
Relation TopologyManager::GetRelation(const std::string& name, pid_t process_id) const {
  const auto writers = GetWriters(name);
  const auto readers = GetReaders(name);
  if (writers.empty() || readers.empty()) return NO_RELATION;
  for (const auto& role : writers) {
    if (role.host_name() != impl_->host_name) return DIFF_HOST;
    if (role.process_id() != process_id) return DIFF_PROC;
  }
  for (const auto& role : readers) {
    if (role.host_name() != impl_->host_name) return DIFF_HOST;
    if (role.process_id() != process_id) return DIFF_PROC;
  }
  return SAME_PROC;
}
bool TopologyManager::IsSameProc(const std::string& name) const {
  const auto writers = GetWriters(name);
  return !writers.empty() && GetRelation(name, writers.front().process_id()) == SAME_PROC;
}
TopologyManager::ChangeConnection TopologyManager::AddChangeListener(ChangeFunc callback) {
  const auto id = impl_->next_listener_id.fetch_add(1);
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->listeners.emplace(id, std::make_shared<Impl::Listener>(std::move(callback)));
  return {id};
}
void TopologyManager::RemoveChangeListener(ChangeConnection connection) {
  std::shared_ptr<Impl::Listener> listener;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->listeners.find(connection.id);
    if (it == impl_->listeners.end()) {
      return;
    }
    listener = it->second;
    impl_->listeners.erase(it);
  }
  Impl::DeactivateListener(listener);
}
void TopologyManager::AddNode(const std::string& name, pid_t process_id) {
  impl_->channel_manager.AddNode(name, process_id);
}
void TopologyManager::AddChannelWriter(const std::string& channel, const std::string& node,
                                       pid_t process_id) {
  impl_->OnChange(impl_->MakeChange(proto::OPT_JOIN, proto::ROLE_WRITER,
                                     impl_->MakeCompatRole(channel, node, process_id)));
}
void TopologyManager::AddChannelReader(const std::string& channel, const std::string& node,
                                       pid_t process_id) {
  impl_->OnChange(impl_->MakeChange(proto::OPT_JOIN, proto::ROLE_READER,
                                     impl_->MakeCompatRole(channel, node, process_id)));
}
bool TopologyManager::HasNode(const std::string& name, pid_t process_id) const {
  return impl_->channel_manager.HasNode(name, process_id);
}
std::string TopologyManager::DumpGraph() const {
  return impl_->channel_manager.DumpGraph();
}

}  // namespace topology
}  // namespace minicyber
