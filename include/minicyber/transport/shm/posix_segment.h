#ifndef MINICYBER_TRANSPORT_SHM_POSIX_SEGMENT_H_
#define MINICYBER_TRANSPORT_SHM_POSIX_SEGMENT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "minicyber/transport/shm/block.h"
#include "minicyber/transport/shm/segment.h"
#include "minicyber/transport/shm/state.h"

namespace minicyber {
namespace transport {

// =============================================================================
// PosixSegment：基于 shm_open + mmap 的共享内存段
//
// 内存布局（与 CyberRT 一致，连续一段 mmap）：
//   [ State ][ Block[block_num] ][ block_buf_0 ][ block_buf_1 ] ... [ block_buf_n ]
//   ^                       ^^^^                              ^^^^
//   state_                  blocks_                            payload 区
//
// 构造参数：
//   channel_id       : 拓扑通道 ID，用于命名 /dev/shm/minicyber_<id>
//   ceiling_msg_size : 单条消息的最大字节数（决定 block_buf_size）
//   block_num        : 消息块数量
//
// Open() 语义：创建或打开（O_CREAT|O_EXCL 失败回退 OpenOnly）。
// Close() 幂等：munmap + 减引用计数。
// Destroy()：Close + shm_unlink，物理销毁。
// =============================================================================

class PosixSegment : public Segment {
 public:
  PosixSegment(uint64_t channel_id, uint64_t ceiling_msg_size = 1024,
               uint32_t block_num = 4);
  ~PosixSegment() override;

  static const char* Type() { return "posix"; }

  // Segment 接口
  bool Open() override;
  void Close() override;
  void Destroy() override;

  void* GetMemPtr() override;
  size_t GetSize() override;

  // 调试/测试辅助
  const std::string& shm_name() const { return shm_name_; }
  State* state() const { return state_; }
  Block* blocks() const { return blocks_; }
  uint32_t block_num() const { return block_num_; }
  uint64_t ceiling_msg_size() const { return ceiling_msg_size_; }
  size_t block_buf_size() const { return ceiling_msg_size_; }

  // ---- 信号处理与注册表（公开供测试访问） ----
  // 注册一个 SHM 名字（仅 OpenOrCreate 成功后调用，表示本进程拥有）
  static void RegisterShmName(const std::string& name);
  // 注销一个 SHM 名字（Destroy 之前调用）
  static void UnregisterShmName(const std::string& name);
  // 安装 SIGINT/SIGTERM/SIGSEGV 处理器（幂等，仅真正安装一次时返回 true）
  static bool InstallSignalHandler();
  // 测试辅助：返回当前已注册名字的副本
  static std::vector<std::string> RegisteredShmNames();
  // 测试辅助：清空注册表（仅供测试调用）
  static void ClearRegistryForTest();
  // 测试辅助：手动触发一次清理（不重新抛信号），返回被 unlink 的名字数
  static int CleanupAllForTest();

 private:
  bool OpenOrCreate();
  bool OpenOnly();

  // 计算段总大小
  size_t TotalSize() const;

  std::string shm_name_;
  uint64_t ceiling_msg_size_;
  uint32_t block_num_;
  size_t total_size_ = 0;

  void* mem_ = nullptr;     // mmap 起始地址
  State* state_ = nullptr;  // 位于 mem_ 偏移 0
  Block* blocks_ = nullptr; // 位于 mem_ + sizeof(State)
  bool opened_ = false;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_SHM_POSIX_SEGMENT_H_