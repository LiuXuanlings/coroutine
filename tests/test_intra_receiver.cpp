#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "minicyber/transport/receiver/intra_receiver.h"
#include "minicyber/transport/transmitter/intra_transmitter.h"

using minicyber::transport::IntraReceiver;
using minicyber::transport::IntraTransmitter;

namespace {
// 记录收到的消息到 vector，便于断言
struct Recorder {
  std::atomic<int> count{0};
  std::vector<std::string> msgs;
  std::mutex mu;
  void OnMsg(const std::shared_ptr<std::string>& msg) {
    std::lock_guard<std::mutex> lg(mu);
    ++count;
    msgs.push_back(*msg);
  }
};
}  // namespace

// Enable + IntraTransmitter::Transmit 后 receiver 回调收到消息
TEST(IntraReceiverTest, TransmitFiresReceiverCallback) {
  const uint64_t CH = 89001;
  Recorder rec;
  IntraReceiver<std::string> rx(CH,
      [&](const std::shared_ptr<std::string>& m) { rec.OnMsg(m); });
  rx.Enable();

  IntraTransmitter<std::string> tx(CH);
  tx.Enable();
  ASSERT_TRUE(tx.Transmit(std::make_shared<std::string>("hello")));

  // 回调在 Dispatch 同步路径上触发，count 应已 >= 1
  EXPECT_GE(rec.count.load(), 1);
  EXPECT_EQ(rec.msgs.back(), "hello");
}

// 未 Enable 时不会收到回调
TEST(IntraReceiverTest, NotEnabledNoCallback) {
  const uint64_t CH = 89002;
  Recorder rec;
  IntraReceiver<std::string> rx(CH,
      [&](const std::shared_ptr<std::string>& m) { rec.OnMsg(m); });
  // 未 Enable
  IntraTransmitter<std::string> tx(CH);
  tx.Enable();
  tx.Transmit(std::make_shared<std::string>("ignored"));
  EXPECT_EQ(rec.count.load(), 0);
}

// Disable 后不再收到回调
TEST(IntraReceiverTest, DisableStopsCallbacks) {
  const uint64_t CH = 89003;
  Recorder rec;
  IntraReceiver<std::string> rx(CH,
      [&](const std::shared_ptr<std::string>& m) { rec.OnMsg(m); });
  rx.Enable();

  IntraTransmitter<std::string> tx(CH);
  tx.Enable();
  tx.Transmit(std::make_shared<std::string>("first"));
  int after_first = rec.count.load();
  EXPECT_GE(after_first, 1);

  rx.Disable();
  tx.Transmit(std::make_shared<std::string>("second"));
  EXPECT_EQ(rec.count.load(), after_first);  // 不再递增
}

// 多个 receiver 在同一 channel 上都收到回调
TEST(IntraReceiverTest, MultipleReceiversAllFired) {
  const uint64_t CH = 89004;
  Recorder r1, r2, r3;
  IntraReceiver<std::string> rx1(CH,
      [&](const std::shared_ptr<std::string>& m) { r1.OnMsg(m); });
  IntraReceiver<std::string> rx2(CH,
      [&](const std::shared_ptr<std::string>& m) { r2.OnMsg(m); });
  IntraReceiver<std::string> rx3(CH,
      [&](const std::shared_ptr<std::string>& m) { r3.OnMsg(m); });
  rx1.Enable();
  rx2.Enable();
  rx3.Enable();

  IntraTransmitter<std::string> tx(CH);
  tx.Enable();
  ASSERT_TRUE(tx.Transmit(std::make_shared<std::string>("broadcast")));

  EXPECT_GE(r1.count.load(), 1);
  EXPECT_GE(r2.count.load(), 1);
  EXPECT_GE(r3.count.load(), 1);
  EXPECT_EQ(r1.msgs.back(), "broadcast");
  EXPECT_EQ(r2.msgs.back(), "broadcast");
  EXPECT_EQ(r3.msgs.back(), "broadcast");
}

// 零拷贝：receiver 收到的 shared_ptr 与 transmit 的是同一对象
TEST(IntraReceiverTest, ZeroCopySameSharedPtr) {
  const uint64_t CH = 89005;
  std::shared_ptr<std::string> captured;
  IntraReceiver<std::string> rx(CH,
      [&](const std::shared_ptr<std::string>& m) { captured = m; });
  rx.Enable();

  IntraTransmitter<std::string> tx(CH);
  tx.Enable();
  auto msg = std::make_shared<std::string>("ptr-identity");
  tx.Transmit(msg);
  ASSERT_TRUE(captured != nullptr);
  EXPECT_EQ(captured.get(), msg.get());
}

// 多次 Transmit，receiver 按序收到每条
TEST(IntraReceiverTest, MultipleTransmitsInOrder) {
  const uint64_t CH = 89006;
  Recorder rec;
  IntraReceiver<std::string> rx(CH,
      [&](const std::shared_ptr<std::string>& m) { rec.OnMsg(m); });
  rx.Enable();

  IntraTransmitter<std::string> tx(CH);
  tx.Enable();
  for (int i = 0; i < 5; ++i) {
    tx.Transmit(std::make_shared<std::string>(std::to_string(i)));
  }
  ASSERT_EQ(rec.msgs.size(), 5u);
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(rec.msgs[i], std::to_string(i));
  }
}

// channel 隔离：CH_A 的 transmit 不触发 CH_B 的 receiver
TEST(IntraReceiverTest, ChannelIsolation) {
  const uint64_t CH_A = 89007, CH_B = 89008;
  Recorder ra, rb;
  IntraReceiver<std::string> rxa(CH_A,
      [&](const std::shared_ptr<std::string>& m) { ra.OnMsg(m); });
  IntraReceiver<std::string> rxb(CH_B,
      [&](const std::shared_ptr<std::string>& m) { rb.OnMsg(m); });
  rxa.Enable();
  rxb.Enable();

  IntraTransmitter<std::string> txa(CH_A), txb(CH_B);
  txa.Enable();
  txb.Enable();
  txa.Transmit(std::make_shared<std::string>("A"));
  txb.Transmit(std::make_shared<std::string>("B"));

  EXPECT_GE(ra.count.load(), 1);
  EXPECT_GE(rb.count.load(), 1);
  EXPECT_EQ(ra.msgs.back(), "A");
  EXPECT_EQ(rb.msgs.back(), "B");
}