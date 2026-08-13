#ifndef MINICYBER_TRANSPORT_SHM_SEGMENT_H_
#define MINICYBER_TRANSPORT_SHM_SEGMENT_H_

#include <cstddef>
#include <cstdint>
#include <memory>

#include "minicyber/transport/shm/block.h"
#include "minicyber/transport/shm/state.h"

namespace minicyber {
namespace transport {

class Segment;
using SegmentPtr = std::shared_ptr<Segment>;

// =============================================================================
// WritableBlock / ReadableBlock
//
// Segment 在分配/读取一条消息时填入此结构，交给上层使用：
//   - index : 该 Block 在 Segment 内的索引
//   - block : 指向共享内存中的 Block 控制元数据
//   - buf   : 指向 Block 之后连续的 payload 缓冲
// 写入流程：AcquireBlockToWrite -> 拷贝数据到 buf -> ReleaseWrittenBlock
// 读取流程：AcquireBlockToRead  -> 从 buf 读数据   -> ReleaseReadBlock
// =============================================================================

struct WritableBlock {
  uint32_t index = 0;
  Block* block = nullptr;
  uint8_t* buf = nullptr;
};
using ReadableBlock = WritableBlock;

// =============================================================================
// Segment 抽象接口
//
// 对外暴露的共享内存段契约。具体后端（PosixSegment/XsiSegment）实现：
//   - Open()      : 打开或创建共享内存段并映射进进程地址空间
//   - Close()     : 解除映射并释放资源（可重复调用，幂等）
//   - GetMemPtr() : 返回映射后的内存起始地址
//   - GetSize()   : 返回段的总字节数
//   - Destroy()   : 物理销毁共享内存（shm_unlink 等），慎用
//
// 上层 AcquireBlockToWrite/Read 等高层 API 在 CyberRT 中由 Segment 基类实现，
// 这里仅保留抽象接口，具体逻辑在 Step 18 PosixSegment 中落地。
// =============================================================================

class Segment {
 public:
  explicit Segment(uint64_t channel_id) : channel_id_(channel_id) {}
  virtual ~Segment() = default;

  // 生命周期接口
  virtual bool Open() = 0;
  virtual void Close() = 0;
  virtual void Destroy() = 0;

  // 内存访问接口
  virtual void* GetMemPtr() = 0;
  virtual size_t GetSize() = 0;

  // Block ownership remains with the segment. A successful acquire grants the
  // caller a temporary view which must be returned through the matching
  // release function before the segment can reuse that block.
  virtual bool AcquireBlockToWrite(size_t msg_size,
                                   WritableBlock* writable_block) = 0;
  virtual void ReleaseWrittenBlock(const WritableBlock& writable_block) = 0;
  virtual bool AcquireBlockToRead(uint32_t index,
                                  ReadableBlock* readable_block) = 0;
  virtual void ReleaseReadBlock(const ReadableBlock& readable_block) = 0;

  // 元信息
  uint64_t channel_id() const { return channel_id_; }

 protected:
  uint64_t channel_id_;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_SHM_SEGMENT_H_
