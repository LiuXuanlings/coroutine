// =============================================================================
// talker：MiniCyber 发布端示例（对齐 CyberRT examples/cyber/talker.cc）
//
// 用法：
//   ./examples/talker
//
// 行为：
//   创建 Node("talker")，在 "/chatter" 通道上每秒发布一条字符串消息，
//   共发布 10 条后退出。
//
// 路由：
//   talker 进程注册 writer（本进程 pid），listener 进程注册 reader（另一 pid），
//   TopologyManager::IsSameProc 返回 false -> Transport 自动选 SHM 后端，
//   跨进程通过共享内存零拷贝传输。
//
// 与 CyberRT 的差异：
//   - 去掉 protobuf/Init/AsyncShutdown/Rate 框架，用 std::this_thread::sleep
//   - 消息类型为 std::string，直接 Write
// =============================================================================

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "minicyber/node/node.h"
#include "minicyber/node/writer.h"

using minicyber::node::Node;

int main() {
  Node node("talker");
  auto writer = node.CreateWriter<std::string>("/chatter");
  if (writer == nullptr) {
    std::cerr << "Failed to create writer on /chatter" << std::endl;
    return 1;
  }

  std::cout << "[talker] publishing on /chatter ..." << std::endl;
  for (int i = 0; i < 10; ++i) {
    auto msg = std::make_shared<std::string>("hello " + std::to_string(i));
    if (writer->Write(msg)) {
      std::cout << "[talker] sent: " << *msg << std::endl;
    } else {
      std::cerr << "[talker] write failed at iteration " << i << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::cout << "[talker] done." << std::endl;
  return 0;
}