#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

#include <sys/syscall.h>
#include <unistd.h>

#include "minicyber/component/component.h"
#include "minicyber/node/node.h"
#include "minicyber/scheduler/scheduler.h"

namespace {

using TestMessage = minicyber::proto::RoleAttributes;

TestMessage MakeMessage(const std::string& value) {
  TestMessage message;
  message.set_node_name(value);
  return message;
}

bool WaitFor(const std::atomic<int>& count, int expected,
             std::condition_variable& ready, std::mutex& mutex) {
  std::unique_lock<std::mutex> lock(mutex);
  return ready.wait_for(lock, std::chrono::seconds(2), [&] {
    return count.load(std::memory_order_acquire) >= expected;
  });
}

class SingleChannelComponent
    : public minicyber::component::Component<TestMessage> {
 public:
  SingleChannelComponent(std::string* output, std::atomic<int>* count,
                         std::atomic<pid_t>* proc_tid,
                         std::condition_variable* ready)
      : output_(output), count_(count), proc_tid_(proc_tid), ready_(ready) {}

 protected:
  bool Init() override { return true; }
  bool Proc(const std::shared_ptr<TestMessage>& msg) override {
    if (output_) *output_ = msg->node_name();
    if (proc_tid_) {
      proc_tid_->store(static_cast<pid_t>(::syscall(SYS_gettid)),
                       std::memory_order_release);
    }
    if (count_) count_->fetch_add(1, std::memory_order_release);
    if (ready_) ready_->notify_all();
    return true;
  }

 private:
  std::string* output_;
  std::atomic<int>* count_;
  std::atomic<pid_t>* proc_tid_;
  std::condition_variable* ready_;
};

class DualChannelComponent
    : public minicyber::component::Component<TestMessage, TestMessage> {
 public:
  DualChannelComponent(std::string* result, std::atomic<int>* count,
                       std::condition_variable* ready)
      : result_(result), count_(count), ready_(ready) {}

 protected:
  bool Init() override { return true; }
  bool Proc(const std::shared_ptr<TestMessage>& primary,
            const std::shared_ptr<TestMessage>& secondary) override {
    *result_ = primary->node_name() + ":" + secondary->node_name();
    count_->fetch_add(1, std::memory_order_release);
    ready_->notify_all();
    return true;
  }

 private:
  std::string* result_;
  std::atomic<int>* count_;
  std::condition_variable* ready_;
};

class SelfClosingComponent
    : public minicyber::component::Component<TestMessage> {
 public:
  SelfClosingComponent(std::atomic<int>* count, std::condition_variable* ready)
      : count_(count), ready_(ready) {}

 protected:
  bool Init() override { return true; }
  bool Proc(const std::shared_ptr<TestMessage>&) override {
    count_->fetch_add(1, std::memory_order_release);
    Shutdown();
    ready_->notify_all();
    return true;
  }

 private:
  std::atomic<int>* count_;
  std::condition_variable* ready_;
};

class NoReaderComponent
    : public minicyber::component::Component<TestMessage> {
 protected:
  bool Init() override { return true; }
  bool Proc(const std::shared_ptr<TestMessage>&) override { return true; }
};

TEST(ComponentTest, SingleChannelProcRunsInProcessorCRoutine) {
  minicyber::scheduler::SchedulerConf scheduler_conf;
  scheduler_conf.thread_num = 1;
  minicyber::scheduler::Scheduler scheduler(scheduler_conf);

  std::string received;
  std::atomic<int> count{0};
  std::atomic<pid_t> proc_tid{-1};
  std::condition_variable ready;
  std::mutex ready_mutex;
  auto comp = std::make_shared<SingleChannelComponent>(
      &received, &count, &proc_tid, &ready);
  minicyber::node::Node publisher("component_single_publisher");
  auto writer = publisher.CreateWriter<TestMessage>("/component_single");
  ASSERT_NE(writer, nullptr);

  minicyber::proto::ComponentConfig config;
  config.set_name("component_single");
  config.add_readers()->set_channel("/component_single");
  ASSERT_TRUE(comp->Initialize(config));

  const pid_t publisher_tid = static_cast<pid_t>(::syscall(SYS_gettid));
  ASSERT_TRUE(writer->Write(MakeMessage("first")));
  ASSERT_TRUE(WaitFor(count, 1, ready, ready_mutex));
  EXPECT_EQ(received, "first");
  EXPECT_NE(proc_tid.load(std::memory_order_acquire), publisher_tid);

  comp->Shutdown();
  ASSERT_TRUE(writer->Write(MakeMessage("after_shutdown")));
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_EQ(count.load(std::memory_order_acquire), 1);
  scheduler.Shutdown();
}

TEST(ComponentTest, DualChannelUsesFirstReaderAsAllLatestDriver) {
  minicyber::scheduler::SchedulerConf scheduler_conf;
  scheduler_conf.thread_num = 1;
  minicyber::scheduler::Scheduler scheduler(scheduler_conf);

  std::string result;
  std::atomic<int> count{0};
  std::condition_variable ready;
  std::mutex ready_mutex;
  auto comp = std::make_shared<DualChannelComponent>(&result, &count, &ready);
  minicyber::node::Node publisher("component_dual_publisher");
  auto primary = publisher.CreateWriter<TestMessage>("/component_primary");
  auto secondary = publisher.CreateWriter<TestMessage>("/component_secondary");
  ASSERT_NE(primary, nullptr);
  ASSERT_NE(secondary, nullptr);

  minicyber::proto::ComponentConfig config;
  config.set_name("component_dual");
  config.add_readers()->set_channel("/component_primary");
  config.add_readers()->set_channel("/component_secondary");
  ASSERT_TRUE(comp->Initialize(config));

  ASSERT_TRUE(primary->Write(MakeMessage("primary_without_secondary")));
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_EQ(count.load(std::memory_order_acquire), 0);
  ASSERT_TRUE(secondary->Write(MakeMessage("vehicle_state")));
  ASSERT_TRUE(primary->Write(MakeMessage("camera")));
  ASSERT_TRUE(WaitFor(count, 1, ready, ready_mutex));
  EXPECT_EQ(result, "camera:vehicle_state");

  comp->Shutdown();
  scheduler.Shutdown();
}

TEST(ComponentTest, CallbackCanShutdownItsOwnComponentWithoutWaiting) {
  minicyber::scheduler::SchedulerConf scheduler_conf;
  scheduler_conf.thread_num = 1;
  minicyber::scheduler::Scheduler scheduler(scheduler_conf);

  std::atomic<int> count{0};
  std::condition_variable ready;
  std::mutex ready_mutex;
  auto comp = std::make_shared<SelfClosingComponent>(&count, &ready);
  minicyber::node::Node publisher("component_self_close_publisher");
  auto writer = publisher.CreateWriter<TestMessage>("/component_self_close");
  ASSERT_NE(writer, nullptr);
  minicyber::proto::ComponentConfig config;
  config.set_name("component_self_close");
  config.add_readers()->set_channel("/component_self_close");
  ASSERT_TRUE(comp->Initialize(config));

  ASSERT_TRUE(writer->Write(MakeMessage("close")));
  ASSERT_TRUE(WaitFor(count, 1, ready, ready_mutex));
  EXPECT_TRUE(comp->IsShutdown());
  scheduler.Shutdown();
}

TEST(ComponentTest, NoReaderConfigFails) {
  minicyber::scheduler::SchedulerConf scheduler_conf;
  scheduler_conf.thread_num = 1;
  minicyber::scheduler::Scheduler scheduler(scheduler_conf);
  auto comp = std::make_shared<NoReaderComponent>();
  minicyber::proto::ComponentConfig config;
  config.set_name("component_without_reader");
  EXPECT_FALSE(comp->Initialize(config));
  scheduler.Shutdown();
}

}  // namespace
