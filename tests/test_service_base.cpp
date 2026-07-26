#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "minicyber/common/types.h"
#include "minicyber/service/client_base.h"
#include "minicyber/service/service_base.h"
#include "minicyber/transport/common/identity.h"
#include "minicyber/transport/message/message_info.h"

// =============================================================================
// Identity 测试
// =============================================================================

using minicyber::transport::Identity;
using minicyber::transport::ID_SIZE;
using minicyber::transport::MessageInfo;

TEST(IdentityTest, DefaultCtorGeneratesNonzeroHash) {
  Identity id;
  EXPECT_NE(id.HashValue(), 0u);
}

TEST(IdentityTest, ExplicitFalseGeneratesZeroHash) {
  Identity id(false);
  EXPECT_EQ(id.HashValue(), 0u);
}

TEST(IdentityTest, CopyCtorPreservesData) {
  Identity a;
  Identity b(a);
  EXPECT_EQ(a.HashValue(), b.HashValue());
  EXPECT_EQ(a, b);
}

TEST(IdentityTest, AssignmentSameValue) {
  Identity a;
  Identity b(false);
  b = a;
  EXPECT_EQ(a, b);
}

TEST(IdentityTest, EqualityOperators) {
  Identity a;
  Identity b(a);
  EXPECT_TRUE(a == b);
  EXPECT_FALSE(a != b);

  Identity c;  // different random value
  // Very unlikely to collide with 8 bytes of randomness
  EXPECT_TRUE(a != c);
  EXPECT_FALSE(a == c);
}

TEST(IdentityTest, ToStringIsNonemptyHex) {
  Identity id;
  std::string s = id.ToString();
  EXPECT_FALSE(s.empty());
  EXPECT_EQ(s.size(), ID_SIZE * 2u);
  // all chars should be hex digits
  for (char c : s) {
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'));
  }
}

TEST(IdentityTest, LengthReturnsIdSize) {
  Identity id;
  EXPECT_EQ(id.Length(), ID_SIZE);
  EXPECT_EQ(id.Length(), 8u);
}

TEST(IdentityTest, SetDataRoundtrip) {
  Identity id(false);
  const char test_data[ID_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  id.set_data(test_data);

  EXPECT_EQ(std::memcmp(id.data(), test_data, ID_SIZE), 0);
  // hash should be recomputed
  EXPECT_NE(id.HashValue(), 0u);
  // deterministic: same data produces same hash
  Identity id2(false);
  id2.set_data(test_data);
  EXPECT_EQ(id.HashValue(), id2.HashValue());
}

// =============================================================================
// MessageInfo 测试
// =============================================================================

TEST(MessageInfoTest, DefaultCtor) {
  MessageInfo info;
  EXPECT_EQ(info.seq_num(), 0u);
  EXPECT_EQ(info.channel_id(), 0u);
  // default Identities should be false (no generation)
  // But wait — Identity default ctor generates... So they WILL be random.
  // That's fine; we just check they exist and are default-constructible
}

TEST(MessageInfoTest, CtorSenderAndSeq) {
  Identity sender;
  MessageInfo info(sender, 42);
  EXPECT_EQ(info.sender_id(), sender);
  EXPECT_EQ(info.seq_num(), 42u);
  EXPECT_EQ(info.channel_id(), 0u);
}

TEST(MessageInfoTest, CtorWithSpareId) {
  Identity sender;
  Identity spare;
  MessageInfo info(sender, 99, spare);
  EXPECT_EQ(info.sender_id(), sender);
  EXPECT_EQ(info.seq_num(), 99u);
  EXPECT_EQ(info.spare_id(), spare);
}

TEST(MessageInfoTest, CopyCtorPreservesAllFields) {
  Identity sender;
  Identity spare;
  MessageInfo original(sender, 123, spare);
  original.set_channel_id(456);

  MessageInfo copied(original);
  EXPECT_EQ(copied.sender_id(), original.sender_id());
  EXPECT_EQ(copied.seq_num(), original.seq_num());
  EXPECT_EQ(copied.spare_id(), original.spare_id());
  EXPECT_EQ(copied.channel_id(), original.channel_id());
}

TEST(MessageInfoTest, AssignmentPreservesAllFields) {
  Identity sender;
  Identity spare;
  MessageInfo a(sender, 777, spare);
  a.set_channel_id(888);

  MessageInfo b;  // default
  b = a;
  EXPECT_EQ(b, a);
}

TEST(MessageInfoTest, EqualityOperators) {
  Identity sender, spare;
  MessageInfo a(sender, 1, spare);
  a.set_channel_id(10);

  MessageInfo b(sender, 1, spare);
  b.set_channel_id(10);
  EXPECT_TRUE(a == b);
  EXPECT_FALSE(a != b);

  MessageInfo c(Identity(), 2, Identity());  // different seq
  EXPECT_TRUE(a != c);
}

TEST(MessageInfoTest, SettersAndGetters) {
  MessageInfo info;
  Identity sender, spare;

  info.set_sender_id(sender);
  info.set_seq_num(555);
  info.set_spare_id(spare);
  info.set_channel_id(666);

  EXPECT_EQ(info.sender_id(), sender);
  EXPECT_EQ(info.seq_num(), 555u);
  EXPECT_EQ(info.spare_id(), spare);
  EXPECT_EQ(info.channel_id(), 666u);
}

// =============================================================================
// ServiceBase 测试
// =============================================================================

using minicyber::service::ServiceBase;

namespace {

class MockService : public ServiceBase {
 public:
  explicit MockService(const std::string& name)
      : ServiceBase(name) {}
  void Destroy() override { destroyed_ = true; }
  bool destroyed() const { return destroyed_; }
 private:
  bool destroyed_ = false;
};

}  // namespace

TEST(ServiceBaseTest, ServiceName) {
  MockService svc("test_service");
  EXPECT_EQ(svc.service_name(), "test_service");
}

TEST(ServiceBaseTest, DestroyCalled) {
  MockService svc("svc");
  EXPECT_FALSE(svc.destroyed());
  svc.Destroy();
  EXPECT_TRUE(svc.destroyed());
}

// =============================================================================
// ClientBase 测试
// =============================================================================

using minicyber::service::ClientBase;

namespace {

/// 模拟 Client：ServiceIsReady 受控
class MockClient : public ClientBase {
 public:
  explicit MockClient(const std::string& name, bool always_ready = false)
      : ClientBase(name), always_ready_(always_ready) {}

  void Destroy() override { destroyed_ = true; }
  bool destroyed() const { return destroyed_; }

  // 当 always_ready_ 为 true 时，ServiceIsReady 始终返回 true
  // 否则，第 nth_call_ 次后返回 true
  bool ServiceIsReady() const override {
    if (always_ready_) return true;
    return (++call_count_) >= ready_after_;
  }

  void SetReadyAfter(int n) { ready_after_ = n; }

 private:
  bool destroyed_ = false;  // 必须在 Destroy() / destroyed() 之前声明
  bool always_ready_ = false;
  mutable int call_count_ = 0;
  int ready_after_ = 3;     // default: becomes ready after 3 calls
};

}  // namespace

TEST(ClientBaseTest, ServiceName) {
  MockClient cli("my_service");
  EXPECT_EQ(cli.ServiceName(), "my_service");
}

TEST(ClientBaseTest, DestroyCalled) {
  MockClient cli("c");
  EXPECT_FALSE(cli.destroyed());
  cli.Destroy();
  EXPECT_TRUE(cli.destroyed());
}

TEST(ClientBaseTest, WaitForServiceReturnsTrueWhenImmediatelyReady) {
  MockClient cli("s", true);  // always_ready
  EXPECT_TRUE(cli.WaitForServiceNanoseconds(std::chrono::nanoseconds(0)));
}

TEST(ClientBaseTest, WaitForServiceReturnsFalseOnTimeout) {
  MockClient cli("s", false);   // never ready
  cli.SetReadyAfter(999999);    // won't become ready
  // very short timeout — won't poll long enough
  EXPECT_FALSE(cli.WaitForServiceNanoseconds(std::chrono::nanoseconds(1)));
}

TEST(ClientBaseTest, WaitForServiceReturnsTrueWhenServiceBecomesReady) {
  MockClient cli("s", false);
  cli.SetReadyAfter(2);  // becomes ready on 2nd call

  // 50ms should be more than enough for 2 polls at 5ms step
  EXPECT_TRUE(cli.WaitForServiceNanoseconds(std::chrono::milliseconds(50)));
}

TEST(ClientBaseTest, WaitForServiceConvenienceTemplate) {
  MockClient cli("s", true);
  EXPECT_TRUE(cli.WaitForService(std::chrono::milliseconds(0)));
}

TEST(ClientBaseTest, WaitForServiceDefaultInfiniteTimeout) {
  MockClient cli("s", true);
  // Default timeout is -1 (infinite), but should return immediately since ready
  EXPECT_TRUE(cli.WaitForService());
}

// =============================================================================
// SRV_CHANNEL 常量测试
// =============================================================================

TEST(SrvChannelSuffixTest, ConstantsDefined) {
  EXPECT_EQ(std::string(minicyber::SRV_CHANNEL_REQ_SUFFIX), "__SRV__REQUEST");
  EXPECT_EQ(std::string(minicyber::SRV_CHANNEL_RES_SUFFIX), "__SRV__RESPONSE");
}

TEST(SrvChannelSuffixTest, Concatenation) {
  std::string service_name = "MyService";
  std::string req_channel = service_name + minicyber::SRV_CHANNEL_REQ_SUFFIX;
  std::string res_channel = service_name + minicyber::SRV_CHANNEL_RES_SUFFIX;
  EXPECT_EQ(req_channel, "MyService__SRV__REQUEST");
  EXPECT_EQ(res_channel, "MyService__SRV__RESPONSE");
}