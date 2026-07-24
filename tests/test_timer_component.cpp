#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "minicyber/component/timer_component.h"

// =============================================================================
// TickComponent — 定时组件测试桩
//
// 模拟最简单的定时器组件：每次 Proc() 递增一个计数器。
// 测试通过计数器值推断 Proc 被调用的次数和频率。
//
// 设计要点：
//   - 使用 std::atomic<int> 计数（无锁，线程安全）
//   - 间隔足够短（10ms）使得在 50ms 内可以累积多次触发
//   - 断言使用宽松边界（>= 下限）避免 CI 环境时间抖动导致 flaky
// =============================================================================
class TickComponent : public minicyber::component::TimerComponent {
 public:
  int count() const { return count_.load(std::memory_order_relaxed); }

 protected:
  bool Init() override { return true; }

  bool Proc() override {
    count_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

 private:
  std::atomic<int> count_{0};
};

// =============================================================================
// FailInitTimerComponent — Init 失败的定时器组件
// =============================================================================
class FailInitTimerComponent : public minicyber::component::TimerComponent {
 protected:
  bool Init() override { return false; }
  bool Proc() override { return true; }
};

// =============================================================================
// TrackClearTimerComponent — 跟踪 Clear/Shutdown 状态的定时器组件
// =============================================================================
class TrackClearTimerComponent : public minicyber::component::TimerComponent {
 public:
  bool clear_called() const { return clear_called_.load(); }
  bool proc_called() const { return proc_called_.load(); }

 protected:
  bool Init() override { return true; }
  bool Proc() override {
    proc_called_.store(true, std::memory_order_relaxed);
    return true;
  }
  void Clear() override {
    clear_called_.store(true, std::memory_order_relaxed);
  }

 private:
  std::atomic<bool> clear_called_{false};
  std::atomic<bool> proc_called_{false};
};

// =============================================================================
// 测试用例
// =============================================================================

// ---------------------------------------------------------------------------
// 验证正常初始化 + 定时触发计数
// ---------------------------------------------------------------------------
// 启动一个 10ms 间隔的 TimerComponent，等待 50ms 后检查计数。
// 理论值：50ms / 10ms ≈ 5 次（除去启动滞后可能在 4-6 次）。
// 宽松断言：>= 2（考虑 CI 环境可能调度延迟较大）。
// ---------------------------------------------------------------------------
TEST(TimerComponentTest, PeriodicTrigger) {
  auto comp = std::make_shared<TickComponent>();

  minicyber::proto::TimerComponentConfig config;
  config.set_name("ticker");
  config.set_interval(10);  // 10ms

  ASSERT_TRUE(comp->Initialize(config));
  EXPECT_GT(comp->GetInterval(), 0);
  EXPECT_EQ(comp->GetInterval(), 10);

  // 等待 50ms，期间 Proc 应该被多次调用
  std::this_thread::sleep_for(std::chrono::milliseconds(60));

  int count = comp->count();
  EXPECT_GE(count, 2);  // 至少触发 2 次（宽松边界）

  comp->Shutdown();
  EXPECT_TRUE(comp->IsShutdown());
}

// ---------------------------------------------------------------------------
// Shutdown 后 Proc 不再被调用
// ---------------------------------------------------------------------------
// 验证 Shutdown 可以干净停止定时线程，且关闭后计数不再增长。
// 通过"等待 Shutdown 后一段时间"来确认线程已退出。
// ---------------------------------------------------------------------------
TEST(TimerComponentTest, ShutdownStopsTimer) {
  auto comp = std::make_shared<TickComponent>();

  minicyber::proto::TimerComponentConfig config;
  config.set_name("stoppable");
  config.set_interval(5);  // 5ms，快速触发

  ASSERT_TRUE(comp->Initialize(config));
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  int before = comp->count();
  EXPECT_GE(before, 2);  // 确保已经触发过

  // Shutdown 后等待一段时间，计数应不再变化
  comp->Shutdown();
  int after_shutdown = comp->count();

  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  int after_wait = comp->count();
  EXPECT_EQ(after_shutdown, after_wait);
}

// ---------------------------------------------------------------------------
// interval = 0 → Initialize 返回 false
// ---------------------------------------------------------------------------
// 定时周期 0 没有意义，框架应拒绝这种配置。
// 注意：这是组件的防御性检查，而不是 Proto 的字段校验（proto 默认为 0）。
// ---------------------------------------------------------------------------
TEST(TimerComponentTest, ZeroIntervalFails) {
  auto comp = std::make_shared<TickComponent>();

  minicyber::proto::TimerComponentConfig config;
  config.set_name("zero_interval");
  config.set_interval(0);  // 非法间隔

  EXPECT_FALSE(comp->Initialize(config));
}

// ---------------------------------------------------------------------------
// Init 失败 → Initialize 返回 false，不启动定时线程
// ---------------------------------------------------------------------------
TEST(TimerComponentTest, InitFailurePropagates) {
  auto comp = std::make_shared<FailInitTimerComponent>();

  minicyber::proto::TimerComponentConfig config;
  config.set_name("fail_init_timer");
  config.set_interval(10);

  EXPECT_FALSE(comp->Initialize(config));
}

// ---------------------------------------------------------------------------
// 重复 Initialize 仅首次生效
// ---------------------------------------------------------------------------
TEST(TimerComponentTest, DoubleInitializeReturnsFalse) {
  auto comp = std::make_shared<TickComponent>();

  minicyber::proto::TimerComponentConfig config;
  config.set_name("double_init");
  config.set_interval(10);

  ASSERT_TRUE(comp->Initialize(config));
  // 第二次 Initialize 应返回 false
  EXPECT_FALSE(comp->Initialize(config));

  comp->Shutdown();
}

// ---------------------------------------------------------------------------
// Shutdown 幂等性
// ---------------------------------------------------------------------------
TEST(TimerComponentTest, ShutdownIsIdempotent) {
  auto comp = std::make_shared<TickComponent>();

  minicyber::proto::TimerComponentConfig config;
  config.set_name("idempotent_timer");
  config.set_interval(10);

  ASSERT_TRUE(comp->Initialize(config));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  comp->Shutdown();
  EXPECT_TRUE(comp->IsShutdown());

  // 第二次 Shutdown 应无 crash
  comp->Shutdown();
  EXPECT_TRUE(comp->IsShutdown());
}

// ---------------------------------------------------------------------------
// Shutdown 生命周期确认：Clear 被调用
// ---------------------------------------------------------------------------
TEST(TimerComponentTest, ShutdownCallsClear) {
  auto comp = std::make_shared<TrackClearTimerComponent>();

  minicyber::proto::TimerComponentConfig config;
  config.set_name("clear_test");
  config.set_interval(10);

  ASSERT_TRUE(comp->Initialize(config));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  EXPECT_TRUE(comp->proc_called());

  comp->Shutdown();
  EXPECT_TRUE(comp->clear_called());
}

// ---------------------------------------------------------------------------
// 未 Initialize 时 Shutdown 安全
// ---------------------------------------------------------------------------
TEST(TimerComponentTest, ShutdownWithoutInit) {
  auto comp = std::make_shared<TickComponent>();
  // 不调用 Initialize，直接 Shutdown
  comp->Shutdown();
  EXPECT_TRUE(comp->IsShutdown());
}
