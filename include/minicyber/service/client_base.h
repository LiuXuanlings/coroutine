#ifndef MINICYBER_SERVICE_CLIENT_BASE_H_
#define MINICYBER_SERVICE_CLIENT_BASE_H_

#include <chrono>
#include <string>
#include <thread>

// =============================================================================
// ClientBase：客户端基类（对齐 CyberRT ClientBase）
//
// 职责：Client 是 RPC 的客户端抽象。每个 Client 绑定一个 service_name，
//   通过 Request Channel 发送请求，通过 Response Channel 接收响应。
//
// ServiceIsReady 设计（Template Method 模式）：
//   CyberRT 的 ClientBase 直接调用
//   service_discovery::TopologyManager::Instance()
//     ->service_manager()->HasService(service_name_)
//   导致 ClientBase 强依赖 TopologyManager + ServiceManager。
//
//   MiniCyber 的 TopologyManager 不含 service_manager（当前阶段不需要），
//   因此将 ServiceIsReady 设计为纯虚函数。ClientBase 提供的 WaitForService
//   基于 ServiceIsReady() 轮询，使用 Template Method 模式 ——
//   派生类负责实现 ServiceIsReady 的具体检查策略。
//
// WaitForService 的 timeout 语义（与 CyberRT 一致）：
//   - timeout 为负值（默认 -1）：无限等待，直到 ServiceIsReady() 返回 true
//   - timeout = 0：仅检查一次，立即返回
//   - timeout > 0：以 5ms 步长轮询，超时后最后一次检查返回结果
// =============================================================================

namespace minicyber {
namespace service {

class ClientBase {
 public:
  explicit ClientBase(const std::string& service_name)
      : service_name_(service_name) {}

  virtual ~ClientBase() = default;

  /// 销毁客户端，释放所有资源（由派生模板实现具体清理）
  virtual void Destroy() = 0;

  /// 检查目标 Service 是否已就绪（纯虚，派生类实现具体检查策略）
  virtual bool ServiceIsReady() const = 0;

  /// 获取服务名
  const std::string& ServiceName() const { return service_name_; }

  /// 以纳秒为单位轮询等待服务就绪
  /// @param timeout 超时时间（纳秒）。负值表示无限等待。
  /// @return true  服务已就绪
  /// @return false 超时
  bool WaitForServiceNanoseconds(std::chrono::nanoseconds timeout) {
    constexpr auto step = std::chrono::nanoseconds(5 * 1000 * 1000);  // 5ms
    while (timeout.count() > 0) {
      if (ServiceIsReady()) {
        return true;
      }
      std::this_thread::sleep_for(step);
      timeout -= step;
    }
    // 超时或初始值为 0：做最后一次检查
    return ServiceIsReady();
  }

  /// 便捷模板方法：等待服务就绪（任意时间单位）
  /// @tparam RatioT 时间单位，默认为 std::milli
  /// @param timeout 超时时间，负值（默认 -1）表示无限等待
  /// @return true  服务已就绪
  /// @return false 超时
  template <typename RatioT = std::milli>
  bool WaitForService(std::chrono::duration<int64_t, RatioT> timeout =
                          std::chrono::duration<int64_t, RatioT>(-1)) {
    return WaitForServiceNanoseconds(
        std::chrono::duration_cast<std::chrono::nanoseconds>(timeout));
  }

 protected:
  std::string service_name_;
};

}  // namespace service
}  // namespace minicyber

#endif  // MINICYBER_SERVICE_CLIENT_BASE_H_