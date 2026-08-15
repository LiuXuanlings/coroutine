#include "minicyber/proto/autodrive_runtime.h"

namespace minicyber {
namespace data {

// 业务 DataDispatcher 由常驻 Demo Runtime 唯一实例化；可卸载的
// Component DSO 只引用这份状态，不会复制进程内数据总线。
template class DataDispatcher<proto::CameraFrame>;
template class DataDispatcher<proto::VehicleState>;
template class DataDispatcher<proto::PerceptionObstacle>;
template class DataDispatcher<proto::FusedObstacle>;
template class DataDispatcher<proto::Trajectory>;
template class DataDispatcher<proto::ControlCommand>;

}  // namespace data

namespace runtime {

template <typename Message>
std::shared_ptr<Message> CreateAutodriveMessage() {
  return std::make_shared<Message>();
}

template <typename Message>
std::shared_ptr<Message> CopyAutodriveMessage(const Message& message) {
  return std::make_shared<Message>(message);
}

template std::shared_ptr<proto::PerceptionObstacle>
CreateAutodriveMessage<proto::PerceptionObstacle>();
template std::shared_ptr<proto::FusedObstacle>
CreateAutodriveMessage<proto::FusedObstacle>();
template std::shared_ptr<proto::Trajectory>
CreateAutodriveMessage<proto::Trajectory>();
template std::shared_ptr<proto::ControlCommand>
CreateAutodriveMessage<proto::ControlCommand>();
template std::shared_ptr<proto::ControlCommand>
CopyAutodriveMessage<proto::ControlCommand>(const proto::ControlCommand&);

}  // namespace runtime
}  // namespace minicyber
