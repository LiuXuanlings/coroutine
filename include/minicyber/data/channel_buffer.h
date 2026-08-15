#ifndef MINICYBER_DATA_CHANNEL_BUFFER_H_
#define MINICYBER_DATA_CHANNEL_BUFFER_H_

#include <algorithm>
#include <memory>
#include <vector>

#include "minicyber/data/cache_buffer.h"

namespace minicyber {
namespace data {

// 将 CacheBuffer 与 channel_id 绑定。底层保存 std::shared_ptr<T>，
// 数据链传递共享指针而不复制消息对象；ChannelBuffer 与调用方
// 共享 CacheBuffer 所有权。
template <typename T>
class ChannelBuffer {
 public:
  using BufferType = CacheBuffer<std::shared_ptr<T>>;

  ChannelBuffer(uint64_t channel_id, std::shared_ptr<BufferType> buffer)
      : channel_id_(channel_id), buffer_(std::move(buffer)) {}

  // Fetch by absolute index. On entry, *index==0 means "give me whatever is
  // newest"; otherwise *index is the next position to read after the last entry the caller processed.
  // Returns false when the buffer is empty, or when the caller is already
  // caught up (*index == Tail + 1). When the caller has fallen behind the
  // live window (*index < Head), *index is reset to Tail (skip dropped
  // messages) and the newest element is returned.
  bool Fetch(uint64_t* index, std::shared_ptr<T>& m) { 
    std::lock_guard<std::mutex> lock(buffer_->Mutex());
    if (buffer_->Empty()) {
      return false;
    }
    if (*index == 0) {
      *index = buffer_->Tail();
    } else if (*index == buffer_->Tail() + 1) {
      return false;
    } else if (*index < buffer_->Head()) {
      // Reader fell behind the overwrite window: fast-forward to newest.
      *index = buffer_->Tail();
    }
    m = buffer_->at(*index);
    return true;
  }

  // Return the newest element. False if the buffer is empty.
  bool Latest(std::shared_ptr<T>& m) {
    std::lock_guard<std::mutex> lock(buffer_->Mutex());
    if (buffer_->Empty()) {
      return false;
    }
    m = buffer_->Back();
    return true;
  }

  // Fetch up to fetch_size newest elements, oldest-first. False if empty.
  bool FetchMulti(uint64_t fetch_size, std::vector<std::shared_ptr<T>>* vec) {
    std::lock_guard<std::mutex> lock(buffer_->Mutex());
    if (buffer_->Empty()) {
      return false;
    }
    auto num = std::min(buffer_->Size(), fetch_size);
    vec->reserve(num);
    for (auto index = buffer_->Tail() - num + 1; index <= buffer_->Tail();
         ++index) {
      vec->emplace_back(buffer_->at(index));
    }
    return true;
  }

  uint64_t channel_id() const { return channel_id_; }
  std::shared_ptr<BufferType> Buffer() const { return buffer_; }

 private:
  uint64_t channel_id_;
  std::shared_ptr<BufferType> buffer_;
};

}  // namespace data
}  // namespace minicyber

#endif  // MINICYBER_DATA_CHANNEL_BUFFER_H_
