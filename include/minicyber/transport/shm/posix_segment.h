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

// 前置：一个待写入/待读取的块上下文
struct ShmWritableBlock {
  uint32_t index = 0;
  Block* block = nullptr;
  uint8_t* buf = nullptr;
};
using ShmReadableBlock = ShmWritableBlock;

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

  // ---- 块级读写接口（供 ShmDispatcher / ShmTransmitter 使用） ----
  // 申请一个可写块：找到下一个空闲块，加写锁，填入 wb。
  // msg_size 为本次消息字节数，必须 <= block_buf_size()。
  // 成功返回 true，wb->buf 指向 payload 缓冲，写完须调用 ReleaseWrittenBlock。
  bool AcquireBlockToWrite(size_t msg_size, ShmWritableBlock* wb);
  // 释放已写块：设置 block 的 msg_size，释放写锁，使数据对读者可见。
  void ReleaseWrittenBlock(const ShmWritableBlock& wb);
  // 读取指定索引的块：加读锁，填入 rb。失败（块未就绪/锁失败）返回 false。
  bool AcquireBlockToRead(uint32_t index, ShmReadableBlock* rb);
  // 释放已读块：释放读锁。
  void ReleaseReadBlock(const ShmReadableBlock& rb);

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
  // 计算第 index 个 block 的 payload 起始地址
  uint8_t* BlockBufAddr(uint32_t index);

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