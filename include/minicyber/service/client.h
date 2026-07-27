#ifndef MINICYBER_SERVICE_CLIENT_H_
#define MINICYBER_SERVICE_CLIENT_H_

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "minicyber/common/types.h"
#include "minicyber/croutine/croutine.h"
#include "minicyber/node/node.h"
#include "minicyber/scheduler/scheduler.h"
#include "minicyber/service/client_base.h"
#include "minicyber/service/service.h"  // RpcRequest, RpcResponse
#include "minicyber/topology/topology_manager.h"
#include "minicyber/transport/common/identity.h"
#include "minicyber/transport/receiver/receiver.h"
#include "minicyber/transport/transmitter/transmitter.h"
#include "minicyber/transport/transport.h"

// =============================================================================
// MiniCyber Client<Req, Rsp>：基于隐式双向 Channel 的 RPC 客户端
//
// 与 CyberRT 的一致点：
//   - service_name 自动派生 request_channel / response_channel
//   - 维护 pending_requests_（seq_num -> Promise/Future）
//   - 响应通过 spare_id == writer_id_ + seq_num 匹配
//
// 关键差异（协程化）：
//   SendRequest 在 CRoutine 上下文中运行时，不阻塞系统线程等待 future，
//   而是：
//     1. 在 pending_ 中登记 {seq_num, crid, fulfilled=false}
//     2. 将当前 CRoutine 状态置为 DATA_WAIT（必须在 Transmit 之前，保证
//        同步触发 HandleResponse 时 NotifyTask 能命中 DATA_WAIT 分支）
//     3. 调用 Transmit 发出请求（INTRA 下同步触发 Service::HandleRequest，
//        进而同步触发 Client::HandleResponse -> 满足 pending + NotifyTask）
//     4. CRoutine::Yield(DATA_WAIT) 让出执行权
//     5. Scheduler 通过 NotifyTask 把协程状态翻回 READY 并重新入队
//     6. 协程恢复后从 pending_ 取出已 fulfilled 的 response 返回
//
// 阻塞降级（非协程上下文）：
//   若 CRoutine::GetThis() 返回 nullptr（普通线程），走 std::future::wait_for
//   阻塞路径。这是为兼容非调度上下文调用而保留的折衷方案。
//
// 超时（妥协）：
//   当前版本未实现协程级 Timer 唤醒。timeout 参数在协程路径下不生效
//   （依赖响应必然到达的假设）；阻塞降级路径使用 future.wait_for。
//   生产环境应通过 Timer + 协程唤醒实现真正的超时，标记为 TODO。
// =============================================================================

namespace minicyber {
namespace service {

template <typename Request, typename Response>
class Client : public ClientBase {
 public:
  using SharedRequest = std::shared_ptr<Request>;
  using SharedResponse = std::shared_ptr<Response>;
  using Promise = std::promise<SharedResponse>;
  using SharedPromise = std::shared_ptr<Promise>;
  using SharedFuture = std::shared_future<SharedResponse>;

  Client(const std::string& node_name, const std::string& service_name)
      : ClientBase(service_name),
        node_name_(node_name),
        request_channel_(service_name + SRV_CHANNEL_REQ_SUFFIX),
        response_channel_(service_name + SRV_CHANNEL_RES_SUFFIX) {}

  ~Client() override { Destroy(); }

  Client() = delete;
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // 初始化客户端：注册拓扑 + 创建 Request Transmitter 和 Response Receiver。
  bool Init() {
    if (inited_) return true;

    auto* topo = topology::TopologyManager::Instance();
    // Client 是 Request channel 的 writer
    topo->AddChannelWriter(request_channel_, node_name_, ::getpid());
    // Client 是 Response channel 的 reader
    topo->AddChannelReader(response_channel_, node_name_, ::getpid());

    // 创建 Request Transmitter
    request_transmitter_ =
        transport::Transport::CreateTransmitter<RpcRequest<Request>>(
            request_channel_);
    if (request_transmitter_ == nullptr) {
      return false;
    }
    writer_id_ = transport::Identity();  // 生成随机身份

    // 创建 Response Receiver，回调同步触发 HandleResponse
    response_receiver_ = transport::Transport::CreateReceiver<RpcResponse<Response>>(
        response_channel_,
        [this](const std::shared_ptr<RpcResponse<Response>>& rsp) {
          this->HandleResponse(rsp);
        });
    if (response_receiver_ == nullptr) {
      request_transmitter_.reset();
      return false;
    }

    inited_ = true;
    return true;
  }

  void Destroy() override {
    if (!inited_) return;
    inited_ = false;
    if (response_receiver_) {
      response_receiver_->Disable();
      response_receiver_.reset();
    }
    if (request_transmitter_) {
      request_transmitter_->Disable();
      request_transmitter_.reset();
    }
    std::lock_guard<std::mutex> lk(pending_mutex_);
    pending_.clear();
  }

  bool ServiceIsReady() const override { return inited_; }

  // 同步发送请求（协程路径走 Yield，非协程路径走 future.wait_for）
  SharedResponse SendRequest(
      const SharedRequest& request,
      std::chrono::nanoseconds timeout = std::chrono::seconds(5)) {
    if (!inited_) return nullptr;

    uint64_t seq = ++sequence_number_;

    // 构造 RpcRequest 包裹
    auto wrapped = std::make_shared<RpcRequest<Request>>();
    wrapped->payload = *request;
    wrapped->sender_id = writer_id_;
    wrapped->seq_num = seq;

    auto cr = CRoutine::GetThis();
    if (cr != nullptr) {
      // ---- 协程路径：Yield(DATA_WAIT) + NotifyTask 唤醒 ----
      PendingSlot slot;
      slot.crid = cr->id();
      slot.fulfilled = false;
      {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        pending_[seq] = slot;
      }

      // 关键：先置 DATA_WAIT 再 Transmit。
      // INTRA 路径下 Transmit 同步触发 HandleResponse -> NotifyTask，
      // 此时 cr->State() 必须已是 DATA_WAIT 才能命中 SetUpdateFlag 分支。
      cr->SetState(RoutineState::DATA_WAIT);

      request_transmitter_->Transmit(wrapped);

      // 让出执行权。若 HandleResponse 已 NotifyTask，则 UpdateState 会将
      // 协程翻回 READY，下次被调度时从 Yield 返回点继续。
      CRoutine::Yield(RoutineState::DATA_WAIT);

      // 恢复后检查 pending
      std::lock_guard<std::mutex> lk(pending_mutex_);
      auto it = pending_.find(seq);
      if (it == pending_.end()) return nullptr;
      SharedResponse result;
      if (it->second.fulfilled) {
        result = it->second.response;
      }
      pending_.erase(it);
      return result;
    }

    // ---- 阻塞降级路径（普通线程，无 CRoutine）----
    SharedPromise promise = std::make_shared<Promise>();
    SharedFuture fut = promise->get_future();
    {
      std::lock_guard<std::mutex> lk(pending_mutex_);
      pending_promise_[seq] = promise;
    }
    request_transmitter_->Transmit(wrapped);
    // INTRA 同步：Transmit 返回时 HandleResponse 已 set_value
    if (fut.wait_for(timeout) == std::future_status::ready) {
      std::lock_guard<std::mutex> lk(pending_mutex_);
      pending_promise_.erase(seq);
      return fut.get();
    }
    std::lock_guard<std::mutex> lk(pending_mutex_);
    pending_promise_.erase(seq);
    return nullptr;
  }

  // 便捷重载：传值而非 shared_ptr
  SharedResponse SendRequest(
      const Request& request,
      std::chrono::nanoseconds timeout = std::chrono::seconds(5)) {
    auto req = std::make_shared<Request>(request);
    return SendRequest(req, timeout);
  }

  const std::string& request_channel() const { return request_channel_; }
  const std::string& response_channel() const { return response_channel_; }

 private:
  struct PendingSlot {
    uint64_t crid = 0;
    bool fulfilled = false;
    SharedResponse response;
  };

  // 响应到达：匹配 pending 并唤醒协程或满足 promise
  void HandleResponse(const std::shared_ptr<RpcResponse<Response>>& rsp) {
    // 先校验 spare_id 是否是自己（防其他 Client 的响应误匹配）
    if (rsp->spare_id != writer_id_) return;

    std::lock_guard<std::mutex> lk(pending_mutex_);

    // 协程路径
    auto it = pending_.find(rsp->seq_num);
    if (it != pending_.end()) {
      it->second.response = std::make_shared<Response>(rsp->payload);
      it->second.fulfilled = true;
      uint64_t crid = it->second.crid;
      // 唤醒挂起的协程（在 Processor 线程上下文中 Scheduler::GetThis 可见）
      if (auto* s = scheduler::Scheduler::GetThis()) {
        s->NotifyTask(crid);
      }
      return;
    }

    // 阻塞降级路径
    auto pit = pending_promise_.find(rsp->seq_num);
    if (pit != pending_promise_.end()) {
      pit->second->set_value(std::make_shared<Response>(rsp->payload));
      pending_promise_.erase(pit);
    }
  }

  std::string node_name_;
  std::string request_channel_;
  std::string response_channel_;

  std::shared_ptr<transport::Transmitter<RpcRequest<Request>>> request_transmitter_;
  std::shared_ptr<transport::Receiver<RpcResponse<Response>>> response_receiver_;
  transport::Identity writer_id_;
  uint64_t sequence_number_ = 0;

  bool inited_ = false;

  std::mutex pending_mutex_;
  // 协程路径：seq_num -> PendingSlot
  std::unordered_map<uint64_t, PendingSlot> pending_;
  // 阻塞降级路径：seq_num -> Promise
  std::unordered_map<uint64_t, SharedPromise> pending_promise_;
};

}  // namespace service
}  // namespace minicyber

#endif  // MINICYBER_SERVICE_CLIENT_H_