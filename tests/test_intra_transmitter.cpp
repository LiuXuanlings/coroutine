#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

#include "minicyber/data/channel_buffer.h"
#include "minicyber/data/data_notifier.h"
#include "minicyber/transport/transmitter/intra_transmitter.h"

using minicyber::data::ChannelBuffer;
using minicyber::data::DataDispatcher;
using minicyber::data::DataNotifier;
using minicyber::data::Notifier;
using minicyber::transport::IntraTransmitter;

namespace {
// 构造一个 ChannelBuffer 并注册到 DataDispatcher，模拟订阅者
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

// Enable + Transmit 后订阅者通过 ChannelBuffer::Fetch 取到消息
TEST(IntraTransmitterTest, TransmitReachesSubscriber) {
  const uint64_t CH = 87001;
  Sub sub(CH);
  IntraTransmitter<std::string> tx(CH);
  tx.Enable();
  auto msg = std::make_shared<std::string>("hello");
  ASSERT_TRUE(tx.Transmit(msg));

  uint64_t index = 0;
  std::shared_ptr<std::string> got;
  ASSERT_TRUE(sub.cb.Fetch(&index, got));
  EXPECT_EQ(*got, "hello");
}

// 未 Enable 时 Transmit 返回 false
TEST(IntraTransmitterTest, TransmitBeforeEnableReturnsFalse) {
  const uint64_t CH = 87002;
  Sub sub(CH);
  IntraTransmitter<std::string> tx(CH);
  EXPECT_FALSE(tx.Transmit(std::make_shared<std::string>("noop")));
  EXPECT_FALSE(tx.enabled());
}

// Disable 后 Transmit 返回 false
TEST(IntraTransmitterTest, DisableBlocksTransmit) {
  const uint64_t CH = 87003;
  Sub sub(CH);
  IntraTransmitter<std::string> tx(CH);
  tx.Enable();
  ASSERT_TRUE(tx.Transmit(std::make_shared<std::string>("first")));
  tx.Disable();
  EXPECT_FALSE(tx.Transmit(std::make_shared<std::string>("second")));
  EXPECT_FALSE(tx.enabled());
}

// seq_num 随每次成功 Transmit 递增
TEST(IntraTransmitterTest, SeqNumIncrementsPerTransmit) {
  const uint64_t CH = 87004;
  Sub sub(CH);
  IntraTransmitter<std::string> tx(CH);
  tx.Enable();
  EXPECT_EQ(tx.seq_num(), 0u);
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(tx.Transmit(std::make_shared<std::string>(std::to_string(i))));
  }
  EXPECT_EQ(tx.seq_num(), 3u);
}

// 无订阅者时 Transmit 仍返回 true（发布即成功，对齐 CyberRT 语义）
TEST(IntraTransmitterTest, TransmitNoSubscriberStillSucceeds) {
  const uint64_t CH = 87005;  // 无订阅者
  IntraTransmitter<std::string> tx(CH);
  tx.Enable();
  EXPECT_TRUE(tx.Transmit(std::make_shared<std::string>("nobody")));
  EXPECT_EQ(tx.seq_num(), 1u);
}

// 零拷贝：Transmit 的 shared_ptr 与 buffer 中存的是同一对象
TEST(IntraTransmitterTest, ZeroCopySameSharedPtr) {
  const uint64_t CH = 87006;
  Sub sub(CH);
  IntraTransmitter<std::string> tx(CH);
  tx.Enable();
  auto msg = std::make_shared<std::string>("ptr-identity");
  tx.Transmit(msg);
  uint64_t index = 0;
  std::shared_ptr<std::string> got;
  ASSERT_TRUE(sub.cb.Fetch(&index, got));
  EXPECT_EQ(got.get(), msg.get());
}

// 多订阅者：同一 channel 上的所有 ChannelBuffer 都被填充
TEST(IntraTransmitterTest, MultipleSubscribersAllFilled) {
  const uint64_t CH = 87007;
  Sub s1(CH), s2(CH), s3(CH);
  IntraTransmitter<std::string> tx(CH);
  tx.Enable();
  ASSERT_TRUE(tx.Transmit(std::make_shared<std::string>("broadcast")));
  for (auto* sub : std::vector<Sub*>{&s1, &s2, &s3}) {
    uint64_t index = 0;
    std::shared_ptr<std::string> got;
    ASSERT_TRUE(sub->cb.Fetch(&index, got));
    EXPECT_EQ(*got, "broadcast");
  }
}

// Transmit 触发 DataNotifier 回调
TEST(IntraTransmitterTest, TransmitFiresDataNotifier) {
  const uint64_t CH = 87008;
  Sub sub(CH);
  std::atomic<int> fired{0};
  auto notifier = std::make_shared<Notifier>();
  notifier->SetCallback([&]() { ++fired; });
  DataNotifier::Instance()->AddNotifier(CH, notifier);

  IntraTransmitter<std::string> tx(CH);
  tx.Enable();
  ASSERT_TRUE(tx.Transmit(std::make_shared<std::string>("wake")));
  EXPECT_GE(fired.load(), 1);
}

// channel_id 隔离：Transmit 到 CH_A 不影响 CH_B 的订阅者
TEST(IntraTransmitterTest, ChannelIsolation) {
  const uint64_t CH_A = 87009, CH_B = 87010;
  Sub sa(CH_A), sb(CH_B);
  IntraTransmitter<std::string> txa(CH_A), txb(CH_B);
  txa.Enable();
  txb.Enable();
  txa.Transmit(std::make_shared<std::string>("A"));
  txb.Transmit(std::make_shared<std::string>("B"));

  uint64_t ia = 0, ib = 0;
  std::shared_ptr<std::string> ga, gb;
  ASSERT_TRUE(sa.cb.Fetch(&ia, ga));
  ASSERT_TRUE(sb.cb.Fetch(&ib, gb));
  EXPECT_EQ(*ga, "A");
  EXPECT_EQ(*gb, "B");
}

TEST(IntraTransmitterTest, DisableBlocksSubsequentTransmitWithoutSequenceAdvance) {
  const uint64_t CH = 87011;
  IntraTransmitter<std::string> tx(CH);
  tx.Enable();
  ASSERT_TRUE(tx.Transmit(std::make_shared<std::string>("before-disable")));
  tx.Disable();
  EXPECT_FALSE(tx.Transmit(std::make_shared<std::string>("after-disable")));
  EXPECT_EQ(tx.seq_num(), 1u);
}
