#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

#include "minicyber/data/channel_buffer.h"
#include "minicyber/data/data_notifier.h"
#include "minicyber/transport/dispatcher/intra_dispatcher.h"

using minicyber::data::ChannelBuffer;
using minicyber::data::DataNotifier;
using minicyber::data::DataDispatcher;
using minicyber::data::Notifier;
using minicyber::transport::IntraDispatcher;

namespace {
// 构造一个 ChannelBuffer 并注册到 DataDispatcher
struct Sub {
  std::shared_ptr<ChannelBuffer<std::string>::BufferType> buf;
  ChannelBuffer<std::string> cb;
  explicit Sub(uint64_t channel_id, uint64_t capacity = 10)
      : buf(std::make_shared<ChannelBuffer<std::string>::BufferType>(capacity)),
        cb(channel_id, buf) {
    DataDispatcher<std::string>::Instance()->AddBuffer(cb);
  }
};
}  // namespace

// Dispatch 后订阅者能通过 ChannelBuffer::Fetch 取到消息。
// 注意：DataDispatcher::Dispatch 的返回值来自 DataNotifier::Notify，
// 没有 Notifier 注册时返回 false，但 buffer 仍被填充。
// 因此这里不依赖 Dispatch 的返回值，而是用 Fetch 验证数据到达。
TEST(IntraDispatcherTest, DispatchReachesSubscriber) {
  const uint64_t CH = 80001;
  Sub sub(CH);
  auto* intra = IntraDispatcher<std::string>::Instance();
  auto msg = std::make_shared<std::string>("hello");
  intra->Dispatch(CH, msg);

  uint64_t index = 0;
  std::shared_ptr<std::string> got;
  ASSERT_TRUE(sub.cb.Fetch(&index, got));
  EXPECT_EQ(*got, "hello");
}

// 无订阅者时 Dispatch 仍返回 false（Notify 无回调）
TEST(IntraDispatcherTest, DispatchNoSubscriberReturnsFalse) {
  const uint64_t CH = 80002;
  auto* intra = IntraDispatcher<std::string>::Instance();
  auto msg = std::make_shared<std::string>("nobody");
  EXPECT_FALSE(intra->Dispatch(CH, msg));
}

// 多订阅者：同一 channel 上的所有 ChannelBuffer 都被填充
TEST(IntraDispatcherTest, MultipleSubscribersAllFilled) {
  const uint64_t CH = 80003;
  Sub s1(CH), s2(CH), s3(CH);
  auto* intra = IntraDispatcher<std::string>::Instance();
  auto msg = std::make_shared<std::string>("broadcast");
  intra->Dispatch(CH, msg);

  for (auto* sub : std::vector<Sub*>{&s1, &s2, &s3}) {
    uint64_t index = 0;
    std::shared_ptr<std::string> got;
    ASSERT_TRUE(sub->cb.Fetch(&index, got));
    EXPECT_EQ(*got, "broadcast");
  }
}

// 不同 channel 互不干扰
TEST(IntraDispatcherTest, ChannelIsolation) {
  const uint64_t CH_A = 80004, CH_B = 80005;
  Sub sa(CH_A), sb(CH_B);
  auto* intra = IntraDispatcher<std::string>::Instance();
  intra->Dispatch(CH_A, std::make_shared<std::string>("A"));
  intra->Dispatch(CH_B, std::make_shared<std::string>("B"));

  uint64_t ia = 0, ib = 0;
  std::shared_ptr<std::string> ga, gb;
  ASSERT_TRUE(sa.cb.Fetch(&ia, ga));
  ASSERT_TRUE(sb.cb.Fetch(&ib, gb));
  EXPECT_EQ(*ga, "A");
  EXPECT_EQ(*gb, "B");
}

// Dispatch 触发 DataNotifier 回调
TEST(IntraDispatcherTest, DispatchFiresDataNotifier) {
  const uint64_t CH = 80006;
  Sub sub(CH);
  std::atomic<int> fired{0};
  auto notifier = std::make_shared<Notifier>();
  notifier->SetCallback([&]() { ++fired; });
  DataNotifier::Instance()->AddNotifier(CH, notifier);

  auto* intra = IntraDispatcher<std::string>::Instance();
  ASSERT_TRUE(intra->Dispatch(CH, std::make_shared<std::string>("wake")));
  EXPECT_GE(fired.load(), 1);
}

// 多次 Dispatch 顺序写入 buffer，FetchMulti 按序读出
TEST(IntraDispatcherTest, MultipleDispatchInOrder) {
  const uint64_t CH = 80007;
  Sub sub(CH, 100);
  auto* intra = IntraDispatcher<std::string>::Instance();
  for (int i = 0; i < 5; ++i) {
    intra->Dispatch(CH, std::make_shared<std::string>(std::to_string(i)));
  }
  std::vector<std::shared_ptr<std::string>> vec;
  ASSERT_TRUE(sub.cb.FetchMulti(5, &vec));
  ASSERT_EQ(vec.size(), 5u);
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(*vec[i], std::to_string(i));
  }
}

// 零拷贝：Dispatch 的 shared_ptr 与 buffer 中存的是同一对象
TEST(IntraDispatcherTest, ZeroCopySameSharedPtr) {
  const uint64_t CH = 80008;
  Sub sub(CH);
  auto* intra = IntraDispatcher<std::string>::Instance();
  auto msg = std::make_shared<std::string>("ptr-test");
  intra->Dispatch(CH, msg);
  uint64_t index = 0;
  std::shared_ptr<std::string> got;
  ASSERT_TRUE(sub.cb.Fetch(&index, got));
  EXPECT_EQ(got.get(), msg.get());  // 同一指针
}

// Instance 单例：多次获取同一对象
TEST(IntraDispatcherTest, SingletonInstance) {
  auto* a = IntraDispatcher<std::string>::Instance();
  auto* b = IntraDispatcher<std::string>::Instance();
  EXPECT_EQ(a, b);
}

// 不同类型的 IntraDispatcher 是独立单例
TEST(IntraDispatcherTest, PerTypeInstance) {
  auto* s = IntraDispatcher<std::string>::Instance();
  auto* i = IntraDispatcher<int>::Instance();
  EXPECT_NE(reinterpret_cast<void*>(s), reinterpret_cast<void*>(i));
}
