// =============================================================================
// Step 33: Service/Client RPC 单测
//
// 测试策略（TDD）：
//   1. EndToEndRpcInCoroutine: 协程内调用 Client::SendRequest，验证
//      DATA_WAIT -> Yield -> (Service 同步处理 + NotifyTask) -> READY 全链路。
//   2. ServiceIsReady: Init 后 ServiceIsReady 返回 true。
//   3. BlockingFallback: 非协程上下文（普通线程）调用 SendRequest，
//      验证 future.wait_for 阻塞降级路径仍可用。
//   4. MultipleSequentialRequests: 同一 Client 连续发 3 个请求，每个都拿到正确响应。
//   5. UnknownSeqNumIgnored: 构造 spare_id 不匹配的伪造响应，验证 pending 不被消费。
//
// 拓扑预注册：所有测试在 CreateService/Client 之前向 TopologyManager 注册
//   Request/Response 两个 channel 的 writer+reader（同 pid），强制走 INTRA，
//   保证 IntraTransmitter::Transmit 同步触发 HandleResponse。
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include "minicyber/croutine/croutine.h"
#include "minicyber/node/node.h"
#include "minicyber/scheduler/scheduler.h"
#include "minicyber/service/client.h"
#include "minicyber/service/service.h"
#include "minicyber/topology/topology_manager.h"

using minicyber::CRoutine;
using minicyber::RoutineState;
using minicyber::node::Node;
using minicyber::scheduler::Scheduler;
using minicyber::scheduler::SchedulerConf;
using minicyber::service::Client;
using minicyber::service::Service;
using minicyber::topology::TopologyManager;

namespace {

const pid_t kPid = ::getpid();

// 简单 Req/Rsp 类型（POD 即可，INTRA 路径零拷贝）
struct AddReq {
  int a = 0;
  int b = 0;
};
struct AddRsp {
  int sum = 0;
};

// 在 TopologyManager 中预注册 Request/Response 两个 channel 的 writer+reader
// 同 pid -> IsSameProc true -> INTRA -> Transmit 同步触发回调
void PreRegisterTopology(const std::string& service_name,
                         const std::string& server_node,
                         const std::string& client_node) {
  auto* topo = TopologyManager::Instance();
  const std::string req_ch = service_name + minicyber::SRV_CHANNEL_REQ_SUFFIX;
  const std::string res_ch = service_name + minicyber::SRV_CHANNEL_RES_SUFFIX;
  topo->AddChannelWriter(req_ch, server_node, kPid);  // Service 端发 Response (writer)
  topo->AddChannelReader(req_ch, client_node, kPid);  // Client 端发 Request (reader of req? no)
  // 实际：Request channel 上 Client=writer, Service=reader
  //       Response channel 上 Service=writer, Client=reader
  // 这里全部注册成 writer+reader 同 pid 即可让 IsSameProc 返回 true
  topo->AddChannelWriter(res_ch, server_node, kPid);
  topo->AddChannelReader(res_ch, client_node, kPid);
}

}  // namespace

// =============================================================================
// 1. 端到端：协程内 RPC，验证 Yield(DATA_WAIT) -> NotifyTask -> READY
// =============================================================================
TEST(ServiceClientTest, EndToEndRpcInCoroutine) {
  const std::string svc_name = "/test_svc_e2e";
  PreRegisterTopology(svc_name, "e2e_server", "e2e_client");

  SchedulerConf conf;
  conf.thread_num = 1;
  Scheduler sched(conf);

  Node server_node("e2e_server");
  Node client_node("e2e_client");

  auto service = server_node.CreateService<AddReq, AddRsp>(
      svc_name, [](const std::shared_ptr<AddReq>& req,
                   std::shared_ptr<AddRsp>& rsp) {
        rsp = std::make_shared<AddRsp>();
        rsp->sum = req->a + req->b;
      });
  ASSERT_NE(service, nullptr);

  auto client = client_node.CreateClient<AddReq, AddRsp>(svc_name);
  ASSERT_NE(client, nullptr);

  std::atomic<bool> done{false};
  std::atomic<int> result{-999};

  sched.CreateTask(
      [&]() {
        auto req = std::make_shared<AddReq>();
        req->a = 17;
        req->b = 25;
        auto rsp = client->SendRequest(req, std::chrono::seconds(5));
        if (rsp) {
          result.store(rsp->sum);
        }
        done.store(true);
      },
      "rpc_caller", 5);

  // 等待协程完成
  for (int i = 0; i < 500 && !done.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(done.load());
  EXPECT_EQ(result.load(), 42);

  sched.Shutdown();
}

// =============================================================================
// 2. ServiceIsReady 在 Init 后返回 true
// =============================================================================
TEST(ServiceClientTest, ServiceIsReadyAfterInit) {
  const std::string svc_name = "/test_svc_ready";
  PreRegisterTopology(svc_name, "ready_server", "ready_client");

  Node server_node("ready_server");
  Node client_node("ready_client");

  auto service = server_node.CreateService<AddReq, AddRsp>(
      svc_name, [](const std::shared_ptr<AddReq>&, std::shared_ptr<AddRsp>&) {});
  ASSERT_NE(service, nullptr);
  EXPECT_TRUE(service->ServiceIsReady());

  auto client = client_node.CreateClient<AddReq, AddRsp>(svc_name);
  ASSERT_NE(client, nullptr);
  EXPECT_TRUE(client->ServiceIsReady());
}

// =============================================================================
// 3. 非协程上下文：阻塞降级路径（future.wait_for）
// =============================================================================
TEST(ServiceClientTest, BlockingFallbackOutsideCoroutine) {
  const std::string svc_name = "/test_svc_block";
  PreRegisterTopology(svc_name, "block_server", "block_client");

  Node server_node("block_server");
  Node client_node("block_client");

  auto service = server_node.CreateService<AddReq, AddRsp>(
      svc_name, [](const std::shared_ptr<AddReq>& req,
                   std::shared_ptr<AddRsp>& rsp) {
        rsp = std::make_shared<AddRsp>();
        rsp->sum = req->a * req->b;
      });
  ASSERT_NE(service, nullptr);

  auto client = client_node.CreateClient<AddReq, AddRsp>(svc_name);
  ASSERT_NE(client, nullptr);

  // 在普通线程中调用 SendRequest（无 CRoutine 上下文）
  auto req = std::make_shared<AddReq>();
  req->a = 6;
  req->b = 7;

  // INTRA 同步：Transmit 直接触发 HandleResponse -> future 已 ready
  auto rsp = client->SendRequest(req, std::chrono::seconds(5));
  ASSERT_NE(rsp, nullptr);
  EXPECT_EQ(rsp->sum, 42);
}

// =============================================================================
// 4. 连续多次请求：每个都拿到正确响应
// =============================================================================
TEST(ServiceClientTest, MultipleSequentialRequests) {
  const std::string svc_name = "/test_svc_multi";
  PreRegisterTopology(svc_name, "multi_server", "multi_client");

  SchedulerConf conf;
  conf.thread_num = 1;
  Scheduler sched(conf);

  Node server_node("multi_server");
  Node client_node("multi_client");

  auto service = server_node.CreateService<AddReq, AddRsp>(
      svc_name, [](const std::shared_ptr<AddReq>& req,
                   std::shared_ptr<AddRsp>& rsp) {
        rsp = std::make_shared<AddRsp>();
        rsp->sum = req->a + req->b;
      });
  ASSERT_NE(service, nullptr);

  auto client = client_node.CreateClient<AddReq, AddRsp>(svc_name);
  ASSERT_NE(client, nullptr);

  std::atomic<int> completed{0};
  std::atomic<int> sum_acc{0};

  sched.CreateTask(
      [&]() {
        for (int i = 0; i < 3; ++i) {
          auto req = std::make_shared<AddReq>();
          req->a = i;
          req->b = 10;
          auto rsp = client->SendRequest(req, std::chrono::seconds(5));
          if (rsp) {
            sum_acc.fetch_add(rsp->sum);
            completed.fetch_add(1);
          }
        }
      },
      "multi_caller", 5);

  for (int i = 0; i < 500 && completed.load() < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(completed.load(), 3);
  // 0+10 + 1+10 + 2+10 = 33
  EXPECT_EQ(sum_acc.load(), 33);

  sched.Shutdown();
}
