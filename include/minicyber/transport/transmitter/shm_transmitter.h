#ifndef MINICYBER_TRANSPORT_TRANSMITTER_SHM_TRANSMITTER_H_
#define MINICYBER_TRANSPORT_TRANSMITTER_SHM_TRANSMITTER_H_

#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "minicyber/transport/shm/condition_notifier.h"
#include "minicyber/transport/shm/posix_segment.h"
#include "minicyber/transport/transmitter/transmitter.h"

namespace minicyber {
namespace transport {

// =============================================================================
// ShmTransmitter：跨进程发布端（对齐 CyberRT ShmTransmitter 原生写入路径）
//
// 职责：把 std::string payload 拷贝进共享内存块，并通过 ConditionNotifier
//       发出跨进程通知，唤醒对端进程的 ShmDispatcher 后台线程。
//
// 写入路径（对齐 CyberRT ShmTransmitter::Transmit）：
//   Transmit(msg)
//     -> segment_->AcquireBlockToWrite(msg->size(), &wb)
//     -> memcpy(wb.buf, msg->data(), msg->size())
//     -> wb.block->set_msg_size(msg->size())
//     -> segment_->ReleaseWrittenBlock(wb)       // 释放写锁，数据对读者可见
//     -> notifier_->Notify(ReadableInfo{0, wb.index, channel_id_})
//
// 与 CyberRT 的简化：
//   1. 消息类型固定为 std::string（roadmap：先支持 string，预留二进制接口）
//   2. 不做 MessageInfo 序列化（CyberRT 在 payload 后跟一段 MessageInfo），
//      本移植只写 payload 本身，msg_size 由 Block::msg_size() 给出
//   3. host_id 固定为 0（单机测试场景，ShmDispatcher 暂不启用 host_id 过滤）
//
// 生命周期：
//   Enable()  : Open segment + Init notifier，标记 enabled_
//   Disable() : Shutdown notifier + Destroy segment，清除 enabled_
//   ~ShmTransmitter() 自动 Disable
//
// 段参数：ceiling_msg_size=1024, block_num=4（与 PosixSegment 默认一致，
//   ShmDispatcher::AddSegment 用相同默认值 OpenOnly，参数自动同步）
// =============================================================================

class ShmTransmitter : public Transmitter<std::string> {
 public:
  explicit ShmTransmitter(uint64_t channel_id,
                           uint64_t ceiling_msg_size = 1024,
                           uint32_t block_num = 4)
      : Transmitter<std::string>(channel_id),
        ceiling_msg_size_(ceiling_msg_size),
        block_num_(block_num) {}

  ~ShmTransmitter() override { Disable(); }

  void Enable() override {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (enabled_) return;
    segment_ = std::make_shared<PosixSegment>(channel_id_, ceiling_msg_size_,
                                              block_num_);
    if (!segment_->Open()) {
      segment_.reset();
      return;
    }
    notifier_ = std::make_unique<ConditionNotifier>();
    if (!notifier_->Init()) {
      notifier_.reset();
      segment_->Destroy();
      segment_.reset();
      return;
    }
    enabled_ = true;
  }

  void Disable() override {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!enabled_) return;
    enabled_ = false;
    if (notifier_) {
      notifier_->Shutdown();
      notifier_.reset();
    }
    if (segment_) {
      segment_->Destroy();
      segment_.reset();
    }
  }

  bool Transmit(const std::shared_ptr<std::string>& msg) override {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!enabled_ || msg == nullptr || segment_ == nullptr || notifier_ == nullptr) {
      return false;
    }

    ShmWritableBlock wb;
    if (!segment_->AcquireBlockToWrite(msg->size(), &wb)) return false;

    std::memcpy(wb.buf, msg->data(), msg->size());
    wb.block->set_msg_size(msg->size());

    uint32_t block_index = wb.index;
    segment_->ReleaseWrittenBlock(wb);

    ReadableInfo info{0, block_index, channel_id_};
    if (!notifier_->Notify(info)) return false;
    NextSeqNum();
    return true;
  }

  // 测试辅助
  PosixSegment* segment() const { return segment_.get(); }

 private:
  uint64_t ceiling_msg_size_;
  uint32_t block_num_;
  std::shared_ptr<PosixSegment> segment_;
  std::unique_ptr<ConditionNotifier> notifier_;
  std::mutex lifecycle_mutex_;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_TRANSMITTER_SHM_TRANSMITTER_H_
