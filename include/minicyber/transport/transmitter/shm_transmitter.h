#ifndef MINICYBER_TRANSPORT_TRANSMITTER_SHM_TRANSMITTER_H_
#define MINICYBER_TRANSPORT_TRANSMITTER_SHM_TRANSMITTER_H_

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>

#include <google/protobuf/message.h>

#include "minicyber/transport/shm/condition_notifier.h"
#include "minicyber/transport/shm/posix_segment.h"
#include "minicyber/transport/transmitter/transmitter.h"

namespace minicyber {
namespace transport {

// 跨进程 SHM 的公开边界固定为 Protobuf 字节序列。与
// cyber_ref/cyber/transport/transmitter/shm_transmitter.h 一样，写锁只有在
// 序列化完成、块提交后才通知对端；普通 C++ 对象或字符串不能绕过该边界。
template <typename M>
class ShmTransmitter : public Transmitter<M> {
  static_assert(std::is_base_of<google::protobuf::Message, M>::value,
                "ShmTransport messages must derive from google::protobuf::Message");

 public:
  explicit ShmTransmitter(uint64_t channel_id,
                          uint64_t ceiling_msg_size = 64 * 1024,
                          uint32_t block_num = 4)
      : Transmitter<M>(channel_id),
        ceiling_msg_size_(ceiling_msg_size),
        block_num_(block_num) {}
  ~ShmTransmitter() override { Disable(); }

  void Enable() override {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (this->enabled_) return;
    segment_ = std::make_shared<PosixSegment>(this->channel_id_,
                                              ceiling_msg_size_, block_num_);
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
    this->enabled_ = true;
  }

  void Disable() override {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!this->enabled_) return;
    this->enabled_ = false;
    if (notifier_) {
      notifier_->Shutdown();
      notifier_.reset();
    }
    if (segment_) {
      segment_->Destroy();
      segment_.reset();
    }
  }

  bool Transmit(const std::shared_ptr<M>& msg) override {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!this->enabled_ || msg == nullptr || segment_ == nullptr ||
        notifier_ == nullptr) {
      return false;
    }
    std::string payload;
    if (!msg->SerializeToString(&payload)) return false;
    ShmWritableBlock block;
    if (!segment_->AcquireBlockToWrite(payload.size(), &block)) return false;
    if (!payload.empty()) std::memcpy(block.buf, payload.data(), payload.size());
    block.block->set_msg_size(payload.size());
    const uint32_t block_index = block.index;
    segment_->ReleaseWrittenBlock(block);
    if (!notifier_->Notify(ReadableInfo{0, block_index, this->channel_id_})) {
      return false;
    }
    this->NextSeqNum();
    return true;
  }

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
