#ifndef MINICYBER_COMPONENT_TIMER_COMPONENT_H_
#define MINICYBER_COMPONENT_TIMER_COMPONENT_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "minicyber/component/component_base.h"
#include "minicyber/proto/component_conf.pb.h"

// =============================================================================
// MiniCyber TimerComponent — 定时触发组件
//
// 设计目标：对齐 Apollo CyberRT 的 TimerComponent，提供基于固定周期定时
//   触发的组件模型。适用于周期性的数据采集、状态检查、心跳维护等场景。
//
// 与 Component<M0> 的核心差异：
//   - Component<M0> 由数据到达驱动（外因触发）
//   - TimerComponent 由时间流逝驱动（内因触发）
//   - 因此 Proc() 无参数，组件自身决定做什么
//
// CyberRT TimerComponent 使用异步 Timer 对象（基于协程 + 定时器任务队列），
// MiniCyber 简化为 std::thread + condition_variable，原因：
//   1. CyberRT 的 Timer 依赖 Scheduler 的协程调度能力，MiniCyber 的
//      Scheduler 当前不支持 RemoveTask
//   2. std::thread 方案自包含，无外部依赖，当前 Phase 6 优先走通链路
//   3. condition_variable::wait_for 支持间隔等待 + 可打断的 Shutdown
//   4. 面试口径："当前使用 thread 方案保证正确性和简洁性，Phase 7 引入
//      Choreography 调度后可升级为协程 Timer 以节省线程资源"
//
// 线程模型：
//   - 每个 TimerComponent 拥有一个独立的 std::thread
//   - 线程循环：sleep(interval) → Process() → sleep(interval) → ...
//   - Shutdown 时通过 condition_variable 打断 sleep 并 join 线程
//   - 结论：N 个 TimerComponent = N 个线程，面试时可讨论优化方向
//
// 与 ComponentBase 的集成：
//   Initialize(config) → 创建 Node + Init() → 启动定时线程
//   Shutdown()        → is_shutdown_ → 打断等待 → join 线程 → Clear()
// =============================================================================

namespace minicyber {
namespace component {

class TimerComponent : public ComponentBase {
 public:
  TimerComponent() = default;
  ~TimerComponent() override { Shutdown(); }

  /**
   * @brief 初始化定时组件
   *
   * 执行流程：
   *   1. node_.reset(new Node(config.name())) — 注册拓扑节点
   *   2. LoadConfigFiles(config)                — 记录配置路径
   *   3. interval_ = config.interval()           — 保存定时周期
   *   4. if (!Init()) return false               — 业务初始化
   *   5. Start() — 启动定时线程                   — 开始循环触发
   *
   * @param config 定时组件配置（必须包含 interval 字段）
   * @return true  初始化成功
   * @return false 初始化失败（Init() 返回 false 或无 interval）
   */
  bool Initialize(const TimerComponentConfig& config) override {
    if (init_.exchange(true, std::memory_order_acq_rel)) {
      return false;  // 防止重复初始化
    }

    // 创建 Node — 注册到 TopologyManager，便于框架感知
    node_.reset(new node::Node(config.name()));
    LoadConfigFiles(config);

    // 校验定时周期必须有值
    interval_ = config.interval();
    if (interval_ == 0) {
      CleanupInitializationFailure();
      init_.store(false, std::memory_order_release);
      return false;
    }

    // 业务初始化
    if (!Init()) {
      CleanupInitializationFailure();
      init_.store(false, std::memory_order_release);
      return false;
    }

    // 启动定时线程
    Start();
    return true;
  }

  /**
   * @brief 组件关闭（幂等）
   *
   * 重写 ComponentBase::Shutdown，在线程 join 之外还保证：
   *   1. 通过 condition_variable 打断线程 sleep → 线程快速退出
   *   2. thread_.join() 等待线程结束 → 线程资源安全回收
   *
   * 注意：TimerComponent 的 Shutdown 覆盖了 ComponentBase 的 Shutdown。
   * 子类如果重写 Shutdown，必须调用 TimerComponent::Shutdown() 或
   * 至少保证 timer 线程被 join。
   */
  void Shutdown() override {
    if (is_shutdown_.exchange(true, std::memory_order_acq_rel)) {
      return;  // 幂等返回
    }

    // Step 1: 通知定时线程退出
    // 先停止线程再清理资源，避免线程仍持有即将销毁的成员引用
    Stop();

    // Step 2: 派生类清理
    Clear();

    // Step 3: 清理 Reader 资源（如果有）
    for (auto& reader : readers_) {
      reader->Shutdown();
    }
    readers_.clear();

  }

  /**
   * @brief 获取定时间隔
   * @return uint64_t 定时周期（毫秒）
   */
  uint64_t GetInterval() const { return interval_; }

  /**
   * @brief 定时处理入口（非虚转发层）
   *
   * 与 Component<M0>::Process 一致的 NVI 模式：
   *   1. 检查 is_shutdown_ 标志
   *   2. 调用纯虚 Proc()
   *
   * @return true  处理成功
   * @return false 处理失败（Proc 返回 false）
   */
  bool Process() {
    if (is_shutdown_.load(std::memory_order_relaxed)) {
      return true;
    }
    return Proc();
  }

 protected:
  /**
   * @brief 业务处理回调（纯虚，由业务组件重写）
   *
   * 与 Component<M0>::Proc(msg) 不同，TimerComponent 的 Proc 无参数，
   * 组件在此函数中执行周期性操作（如发布传感器数据、状态检查等）。
   *
   * @return true  处理成功
   * @return false 处理失败
   */
  virtual bool Proc() = 0;

  /**
   * @brief 派生类自定义清理
   *
   * 重写 Clear() 以释放派生类持有的资源（如关闭文件、释放 GPU 显存）。
   * 注意：Clear 调用时定时线程已停止，可以安全修改状态。
   */
  void Clear() override {}

 private:
  /**
   * @brief 启动定时线程
   *
   * 线程循环逻辑：
   *   while (!is_shutdown_) {
   *     auto wait_until = now + interval;
   *     cv_.wait_until(wait_until, [is_shutdown])  // 可打断睡眠
   *     if (is_shutdown_) break;
   *     Process();
   *   }
   *
   * 为什么用 wait_until 而不是 sleep_for：
   *   sleep_for 无法被外部打断（除非线程被 kill），导致 Shutdown 必须
   *   "等待当前 sleep 结束"才能 join。max(wait_until) 可能导致最多
   *   interval 毫秒的 Shutdown 延迟。用 condition_variable 可以在
   *   Stop() 中通过 notify 立即打断等待。
   */
  void Start() {
    thread_ = std::thread([this]() {
      while (!is_shutdown_.load(std::memory_order_relaxed)) {
        auto now = std::chrono::steady_clock::now();
        auto wait_until = now + std::chrono::milliseconds(interval_);

        // 可打断睡眠：cv_.wait_until 在 notify_one() 或超时时返回
        std::unique_lock<std::mutex> lk(cv_mutex_);
        cv_.wait_until(lk, wait_until, [this]() {
          return is_shutdown_.load(std::memory_order_relaxed);
        });
        lk.unlock();

        // Shutdown 信号已到达 → 退出循环
        if (is_shutdown_.load(std::memory_order_relaxed)) {
          break;
        }

        // 执行定时回调
        Process();
      }
    });
  }

  /**
   * @brief 停止定时线程
   *
   * 先 notify condition_variable 打断正在 sleep 的线程，
   * 再 join 等待线程结束。两步缺一不可：
   *   1. notify → 线程从 wait_until 退出
   *   2. join  → 确保线程已完全销毁
   */
  void Stop() {
    {
      std::lock_guard<std::mutex> lk(cv_mutex_);
      cv_.notify_one();
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  /// 初始化标志（防止重复 Initialize）
  std::atomic<bool> init_{false};

  /// 定时周期（毫秒）
  uint64_t interval_ = 0;

  /// 定时线程
  std::thread thread_;

  /// 可打断睡眠用的条件变量
  std::condition_variable cv_;
  std::mutex cv_mutex_;
};

}  // namespace component
}  // namespace minicyber

#endif  // MINICYBER_COMPONENT_TIMER_COMPONENT_H_
