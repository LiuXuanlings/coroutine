#ifndef MINICYBER_SERVICE_SERVICE_H_
#define MINICYBER_SERVICE_SERVICE_H_

#include <functional>
#include <memory>
#include <string>

#include "minicyber/common/types.h"
#include "minicyber/node/node.h"
#include "minicyber/service/service_base.h"
#include "minicyber/topology/topology_manager.h"
#include "minicyber/transport/common/identity.h"
#include "minicyber/transport/receiver/receiver.h"
#include "minicyber/transport/transmitter/transmitter.h"
#include "minicyber/transport/transport.h"

// =============================================================================
// MiniCyber Service<Req, Rsp>：基于隐式双向 Channel 的 RPC 服务端
//
// 与 CyberRT 的一致点：
//   - service_name 自动派生两个隐式 channel：
//       request_channel  = service_name + "__SRV__REQUEST"
//       response_channel = service_name + "__SRV__RESPONSE"
//   - Service 监听 Request channel，处理后将 Response 发到 Response channel。
//   - 响应中携带原请求的 sender_id 作为 spare_id，供 Client 匹配。
//
// 与 CyberRT 的差异（受 MiniCyber 传输层约束）：
//   - MiniCyber 的 Transmitter::Transmit(msg) 仅传消息体，无 MessageInfo 旁路。
//     因此将 RPC 元数据（sender_id, seq_num, spare_id）内嵌到包裹消息体
//     RpcRequest<Req> / RpcResponse<Rsp> 中，在业务层模拟虚通道。
//   - Service 不再起独立线程 + 任务队列（CyberRT 的 Process/Enqueue）：
//     MiniCyber 的 INTRA 路径下 IntraTransmitter::Transmit 同步触发回调，
//     在 Client 协程的 Processor 线程内同步执行 HandleRequest 即可，
//     避免引入额外线程与锁。
//
// 回调路径（INTRA 同步）：
//   Client::SendRequest
//     -> request_transmitter_->Transmit(RpcRequest<Req>)
//        -> IntraDispatcher<Req>::Dispatch
//           -> DataDispatcher::Dispatch -> ChannelBuffer::Fill
//           -> DataNotifier::Notify
//             -> IntraReceiver 回调
//               -> Service::HandleRequest(req)
//                 -> service_callback_(req, rsp)
//                 -> response_transmitter_->Transmit(RpcResponse<Rsp>)
//                    -> (同样同步触发 Client::HandleResponse)
//                      -> Client::NotifyTask(crid)  [若 Client 在协程中]
//
// 拓扑注册：
//   Init() 时向 TopologyManager 注册 Response channel 的 writer（Service 自己）
//   和 Request channel 的 reader（Service 自己），同 pid -> INTRA 路由。
// =============================================================================

namespace minicyber {
namespace service {

// RPC 请求包裹：在 Req payload 旁携带 Client 的 sender_id 和递增 seq_num。
// 通过 INTRA 通道直接传递 shared_ptr，零拷贝。
template <typename Req>
struct RpcRequest {
  Req payload;
  transport::Identity sender_id;  // Client 的 writer_id_
  uint64_t seq_num = 0;           // Client 端单调递增的请求序号
};

// RPC 响应包裹：在 Rsp payload 旁携带原请求的 sender_id（回填为 spare_id）
// 和相同的 seq_num，供 Client 端 pending_requests 匹配。
template <typename Rsp>
struct RpcResponse {
  Rsp payload;
  transport::Identity spare_id;   // 回填为 Req::sender_id（Client 的 writer_id_）
  uint64_t seq_num = 0;           // 与对应 Req::seq_num 一致
};

template <typename Request, typename Response>
class Service : public ServiceBase {
 public:
  using ServiceCallback =
      std::function<void(const std::shared_ptr<Request>&, std::shared_ptr<Response>&)>;

  Service(const std::string& node_name, const std::string& service_name,
          const ServiceCallback& callback)
      : ServiceBase(service_name),
        node_name_(node_name),
        service_callback_(callback),
        request_channel_(service_name + SRV_CHANNEL_REQ_SUFFIX),
        response_channel_(service_name + SRV_CHANNEL_RES_SUFFIX) {}

  ~Service() override { Destroy(); }

  Service() = delete;
  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;

  // 初始化服务端：注册拓扑 + 创建 Request Receiver 和 Response Transmitter。
  // 返回 false 表示创建失败。
  bool Init() {
    if (inited_) return true;

    auto* topo = topology::TopologyManager::Instance();
    // Service 是 Response channel 的 writer
    topo->AddChannelWriter(response_channel_, node_name_, ::getpid());
    // Service 是 Request channel 的 reader
    topo->AddChannelReader(request_channel_, node_name_, ::getpid());

    // 创建 Response Transmitter（发布响应）
    response_transmitter_ =
        transport::Transport::CreateTransmitter<RpcResponse<Response>>(
            response_channel_);
    if (response_transmitter_ == nullptr) {
      return false;
    }

    // 创建 Request Receiver（订阅请求）
    // 回调在 INTRA 路径下同步触发：Service::HandleRequest
    request_receiver_ = transport::Transport::CreateReceiver<RpcRequest<Request>>(
        request_channel_,
        [this](const std::shared_ptr<RpcRequest<Request>>& req) {
          this->HandleRequest(req);
        });
    if (request_receiver_ == nullptr) {
      response_transmitter_.reset();
      return false;
    }

    inited_ = true;
    return true;
  }

  // 销毁：释放 Transmitter/Receiver，从拓扑注销（由 TopologyManager 幂等）
  void Destroy() override {
    if (!inited_) return;
    inited_ = false;
    if (request_receiver_) {
      request_receiver_->Disable();
      request_receiver_.reset();
    }
    if (response_transmitter_) {
      response_transmitter_->Disable();
      response_transmitter_.reset();
    }
  }

  // ServiceIsReady：Init 成功即视为就绪
  bool ServiceIsReady() const { return inited_; }

  const std::string& request_channel() const { return request_channel_; }
  const std::string& response_channel() const { return response_channel_; }

 private:
  // 处理一条到达的 Request：调用业务回调填充 Response，发回 Client。
  void HandleRequest(const std::shared_ptr<RpcRequest<Request>>& req) {
    if (!inited_) return;

    // 用 aliasing shared_ptr 暴露 req->payload 作为 const shared_ptr<Request>，
    // 保持零拷贝且控制块与 req 共享（req 不会被提前析构）。
    std::shared_ptr<Request> req_payload(req, &req->payload);

    auto rsp = std::make_shared<Response>();
    service_callback_(req_payload, rsp);

    // 构造 RpcResponse，回填 sender_id 为 spare_id，seq_num 透传
    auto wrapped = std::make_shared<RpcResponse<Response>>();
    wrapped->payload = std::move(*rsp);
    wrapped->seq_num = req->seq_num;
    wrapped->spare_id = req->sender_id;

    response_transmitter_->Transmit(wrapped);
  }

  std::string node_name_;
  ServiceCallback service_callback_;
  std::string request_channel_;
  std::string response_channel_;

  std::shared_ptr<transport::Transmitter<RpcResponse<Response>>> response_transmitter_;
  std::shared_ptr<transport::Receiver<RpcRequest<Request>>> request_receiver_;

  bool inited_ = false;
};

}  // namespace service
}  // namespace minicyber

#endif  // MINICYBER_SERVICE_SERVICE_H_