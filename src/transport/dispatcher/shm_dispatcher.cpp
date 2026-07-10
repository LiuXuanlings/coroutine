#include "minicyber/transport/dispatcher/shm_dispatcher.h"

#include <cstring>
#include <utility>

namespace minicyber {
namespace transport {

ShmDispatcher::ShmDispatcher() { Init(); }

ShmDispatcher::~ShmDispatcher() { Shutdown(); }

void ShmDispatcher::Init() {
  notifier_ = std::make_unique<ConditionNotifier>();
  if (!notifier_->Init()) {
    return;
  }
  running_.store(true);
  thread_ = std::thread(&ShmDispatcher::ThreadFunc, this);
}

void ShmDispatcher::AddSegment(uint64_t channel_id) {
  // 幂等：已存在则不重复添加（避免替换导致旧 PosixSegment 析构 shm_unlink）
  if (segments_.count(channel_id) > 0) return;
  auto seg = std::make_shared<PosixSegment>(channel_id);
  if (!seg->Open()) {
    return;
  }
  segments_[channel_id] = std::move(seg);
}

void ShmDispatcher::ThreadFunc() {
  ReadableInfo info;
  while (running_.load()) {
    // 原生 ConditionNotifier：Listen 返回 ReadableInfo，直接给出目标
    // channel_id 与 block_index，无需扫描所有 segment
    if (!notifier_->Listen(100, &info)) {
      continue;
    }
    // host_id 过滤暂不启用（单机测试场景）
    ReadMessage(info.channel_id, info.block_index);
  }
}

void ShmDispatcher::ReadMessage(uint64_t channel_id, uint32_t block_index) {
  auto it = segments_.find(channel_id);
  if (it == segments_.end() || it->second == nullptr) return;
  auto& seg = it->second;

  ShmReadableBlock rb;
  if (!seg->AcquireBlockToRead(block_index, &rb)) return;

  uint64_t msg_size = rb.block->msg_size();
  if (msg_size > 0 && msg_size <= seg->block_buf_size()) {
    std::string msg(reinterpret_cast<const char*>(rb.buf),
                    static_cast<size_t>(msg_size));
    auto msg_ptr = std::make_shared<std::string>(std::move(msg));
    data::DataDispatcher<std::string>::Instance()->Dispatch(channel_id, msg_ptr);
  }
  seg->ReleaseReadBlock(rb);
}

void ShmDispatcher::Shutdown() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
  if (notifier_) notifier_->Shutdown();
  segments_.clear();
}

}  // namespace transport
}  // namespace minicyber