#ifndef MINICYBER_SERVICE_SERVICE_BASE_H_
#define MINICYBER_SERVICE_SERVICE_BASE_H_

#include <string>

// =============================================================================
// ServiceBase：服务端基类（对齐 CyberRT ServiceBase）
//
// 职责：Service 是 RPC 的服务端抽象。每个 Service 绑定一个 service_name，
//   内部自动创建 Request/Response 两个隐式 Channel
//   （service_name + "__SRV__REQUEST" / "__SRV__RESPONSE"）。
//   Client 发请求到 Request Channel，Service 处理后发响应到 Response Channel。
//
// MiniCyber 与 CyberRT 的差异：
//   - 去掉 ClassLoader / ServiceFactory / role_attributes 相关字段
//   - Destroy 接口在 Step 33 的 Service<Req,Rsp> 中实现具体清理逻辑
//
// 使用方式：
//   class MyService : public minicyber::service::Service<MyReq, MyRsp> {
//     // Step 33 实现 Init() 和 HandleRequest()
//   };
// =============================================================================

namespace minicyber {
namespace service {

class ServiceBase {
 public:
  explicit ServiceBase(const std::string& service_name)
      : service_name_(service_name) {}

  virtual ~ServiceBase() = default;

  /// 销毁服务，释放所有资源（由派生模板实现具体清理）
  virtual void Destroy() = 0;

  /// 获取服务名
  const std::string& service_name() const { return service_name_; }

 protected:
  std::string service_name_;
};

}  // namespace service
}  // namespace minicyber

#endif  // MINICYBER_SERVICE_SERVICE_BASE_H_