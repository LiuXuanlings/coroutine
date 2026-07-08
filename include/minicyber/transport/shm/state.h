#ifndef MINICYBER_TRANSPORT_SHM_STATE_H_
#define MINICYBER_TRANSPORT_SHM_STATE_H_

#include <atomic>
#include <cstdint>

namespace minicyber {
namespace transport {

// =============================================================================
// SHM 全局控制区 (State)
//
// 该结构位于共享内存的头部，记录整个 Segment 的元信息：
//   - need_remap_        : 是否需要重新映射（Segment 重建后通知对端重映射）
//   - seq_              : 全局消息序号（单调递增，用于 Indicator 推进）
//   - reference_count_  : 引用计数（管理 Segment 生命周期）
//   - ceiling_msg_size_ : 该 Segment 允许的最大单条消息大小
//
// 所有字段均为 atomic，可被多进程安全读写。
// =============================================================================

class State {
 public:
  explicit State(const uint64_t& ceiling_msg_size)
      : ceiling_msg_size_(ceiling_msg_size) {}
  virtual ~State() = default;

  // 引用计数 -1，不低于 0（CAS 自旋保证不出现负数）
  void DecreaseReferenceCounts() {
    uint32_t current_reference_count = reference_count_.load();
    do {
      if (current_reference_count == 0) {
        return;
      }
    } while (!reference_count_.compare_exchange_strong(
        current_reference_count, current_reference_count - 1));
  }

  // 引用计数 +1
  void IncreaseReferenceCounts() { reference_count_.fetch_add(1); }

  // 序号原子累加 diff，返回累加前的值
  uint32_t FetchAddSeq(uint32_t diff) { return seq_.fetch_add(diff); }
  uint32_t seq() { return seq_.load(); }

  void set_need_remap(bool need) { need_remap_.store(need); }
  bool need_remap() { return need_remap_.load(); }

  uint64_t ceiling_msg_size() { return ceiling_msg_size_.load(); }
  uint32_t reference_counts() { return reference_count_.load(); }

 private:
  std::atomic<bool> need_remap_ = {false};
  std::atomic<uint32_t> seq_ = {0};
  std::atomic<uint32_t> reference_count_ = {0};
  std::atomic<uint64_t> ceiling_msg_size_;
};

}  // namespace transport
}  // namespace minicyber

#endif  // MINICYBER_TRANSPORT_SHM_STATE_H_