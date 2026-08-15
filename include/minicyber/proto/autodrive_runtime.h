#ifndef MINICYBER_PROTO_AUTODRIVE_RUNTIME_H_
#define MINICYBER_PROTO_AUTODRIVE_RUNTIME_H_

#include <memory>

// 业务消息的 DataDispatcher 状态必须由 minicyber_core 唯一持有。没有 extern
// template 时，mainboard 与 dlopen 的组件库会各自生成同名函数局部单例，拓扑
// 仍可匹配但消息不会进入另一份数据总线；这与 CyberRT 共享传输运行时的职责不符。

#include "minicyber/data/data_dispatcher.h"
#include "minicyber/proto/autodrive.pb.h"

namespace minicyber {
namespace data {

extern template class DataDispatcher<proto::CameraFrame>;
extern template class DataDispatcher<proto::VehicleState>;
extern template class DataDispatcher<proto::PerceptionObstacle>;
extern template class DataDispatcher<proto::FusedObstacle>;
extern template class DataDispatcher<proto::Trajectory>;
extern template class DataDispatcher<proto::ControlCommand>;

}  // namespace data

namespace runtime {

// 组件 DSO 会把消息交给核心数据总线；控制块的销毁代码必须留在
// minicyber_core，才能在 ModuleController::Clear 卸载插件后安全释放
// ChannelBuffer 与观察者仍借用的 shared_ptr。
template <typename Message>
std::shared_ptr<Message> CreateAutodriveMessage();

template <typename Message>
std::shared_ptr<Message> CopyAutodriveMessage(const Message& message);

extern template std::shared_ptr<proto::PerceptionObstacle>
CreateAutodriveMessage<proto::PerceptionObstacle>();
extern template std::shared_ptr<proto::FusedObstacle>
CreateAutodriveMessage<proto::FusedObstacle>();
extern template std::shared_ptr<proto::Trajectory>
CreateAutodriveMessage<proto::Trajectory>();
extern template std::shared_ptr<proto::ControlCommand>
CreateAutodriveMessage<proto::ControlCommand>();
extern template std::shared_ptr<proto::ControlCommand>
CopyAutodriveMessage<proto::ControlCommand>(const proto::ControlCommand&);

}  // namespace runtime
}  // namespace minicyber

#endif  // MINICYBER_PROTO_AUTODRIVE_RUNTIME_H_
