#include "minicyber/proto/autodrive_runtime.h"

namespace minicyber {
namespace data {

// 这些显式实例化把业务 DataDispatcher 的状态和函数地址固定在核心库；
// 组件 .so 只引用它们，不在 dlopen 边界复制数据总线单例。
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
