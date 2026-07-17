// =============================================================================
// listener：MiniCyber 订阅端示例（对齐 CyberRT examples/cyber/listener.cc）
//
// 用法：
//   ./examples/listener
//
// 行为：
//   创建 Node("listener")，在 "/chatter" 通道上订阅消息，
//   回调打印收到的内容。运行 15 秒后退出（等待 talker 发送 10 条）。
//
// 路由：
//   listener 进程注册 reader（本进程 pid），与 talker 进程的 writer 不同 pid，
//   TopologyManager::IsSameProc 返回 false -> Transport 自动选 SHM 后端，
//   ShmDispatcher 后台线程轮询 ConditionNotifier，读取 SHM 块，
//   注入 DataDispatcher -> DataNotifier -> ShmReceiver 回调 -> 打印。
//
// 与 CyberRT 的差异：
//   - 去掉 protobuf/Init/AsyncShutdown 框架，用 std::this_thread::sleep
//   - 回调直接打印 std::string，无反序列化
// =============================================================================

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "minicyber/node/node.h"
#include "minicyber/node/reader.h"

using minicyber::node::Node;

int main() {
  Node node("listener");
  auto reader = node.CreateReader<std::string>(
      "/chatter", [](const std::shared_ptr<std::string>& msg) {
        std::cout << "[listener] received: " << *msg << std::endl;
      });
  if (reader == nullptr) {
    std::cerr << "Failed to create reader on /chatter" << std::endl;
    return 1;
  }

  std::cout << "[listener] listening on /chatter ..." << std::endl;
  // 等待 talker 发送 10 条消息（每秒 1 条，留余量）
  std::this_thread::sleep_for(std::chrono::seconds(15));

  std::cout << "[listener] done." << std::endl;
  return 0;
}