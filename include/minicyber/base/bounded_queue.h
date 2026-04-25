#ifndef MINICYBER_BASE_BOUNDED_QUEUE_H_
#define MINICYBER_BASE_BOUNDED_QUEUE_H_

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>

#include "minicyber/base/macros.h"
#include "minicyber/base/wait_strategy.h"

namespace minicyber {

// =============================================================================
// 无锁有界队列 (Lock-free Bounded Queue)
// 基于三原子变量 (head_, tail_, commit_) 的环形队列实现
//
// 核心设计：
// - head_: 消费位置（已取出）
// - tail_: 生产预留位置（已申请但未写入完成）
// - commit_: 已提交位置（写入完成，对消费者可见）
//
// 生产流程：
//   1. CAS tail_ 预留位置
//   2. 写入 pool_[old_tail]
//   3. CAS commit_ 标记可见
//
// 消费流程：
//   1. CAS head_ 获取位置
//   2. 检查 head+1 < commit_（有数据可读）
//   3. 读取 pool_[new_head]
// =============================================================================
template <typename T>
class BoundedQueue {
 public:
  using value_type = T;
  using size_type = uint64_t;

 public:
  BoundedQueue() = default;
  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;
  ~BoundedQueue();

  // 初始化队列，使用默认 SleepWaitStrategy
  bool Init(uint64_t size);
  // 初始化队列，使用自定义等待策略
  bool Init(uint64_t size, WaitStrategy* strategy);

  // 基本查询接口
  uint64_t Size();
  bool Empty();
  uint64_t Head() { return head_.load(); }
  uint64_t Tail() { return tail_.load(); }
  uint64_t Commit() { return commit_.load(); }

  // 设置等待策略
  void SetWaitStrategy(WaitStrategy* strategy);
  // 中断所有等待
  void BreakAllWait();

  // 非阻塞操作（TODO）
  bool Enqueue(const T& element);
  bool Enqueue(T&& element);
  bool Dequeue(T* element);

  // 阻塞操作（TODO）
  bool WaitEnqueue(const T& element);
  bool WaitEnqueue(T&& element);
  bool WaitDequeue(T* element);

 private:
  // 将全局编号转换为 pool 索引（比 % 取模更快）
  uint64_t GetIndex(uint64_t num);

 private:
  // =============================================================================
  // CACHELINE_SIZE 对齐：消除伪共享 (False Sharing)
  // head_/tail_/commit_ 分别位于不同缓存行，避免多核竞争同一缓存行
  // =============================================================================
  alignas(CACHELINE_SIZE) std::atomic<uint64_t> head_{0};
  alignas(CACHELINE_SIZE) std::atomic<uint64_t> tail_{1};
  alignas(CACHELINE_SIZE) std::atomic<uint64_t> commit_{1};

  uint64_t pool_size_ = 0;           // 实际分配的槽位数（capacity + 2）
  T* pool_ = nullptr;                // 元素存储池
  std::unique_ptr<WaitStrategy> wait_strategy_;
  volatile bool break_all_wait_ = false;  // 中断标志
};

// =============================================================================
// 析构函数：清理资源
// =============================================================================
template <typename T>
BoundedQueue<T>::~BoundedQueue() {
  if (wait_strategy_) {
    BreakAllWait();
  }
  if (pool_) {
    // 显式调用每个元素的析构函数
    for (uint64_t i = 0; i < pool_size_; ++i) {
      pool_[i].~T();
    }
    std::free(pool_);
  }
}

// =============================================================================
// 初始化：使用默认 SleepWaitStrategy
// =============================================================================
template <typename T>
inline bool BoundedQueue<T>::Init(uint64_t size) {
  return Init(size, new SleepWaitStrategy());
}

// =============================================================================
// 初始化：分配内存并构造元素
// size: 用户请求的容量
// pool_size_: 实际分配 size + 2（head 和 tail 各占一个哨兵位）
// =============================================================================
template <typename T>
bool BoundedQueue<T>::Init(uint64_t size, WaitStrategy* strategy) {
  // Head 和 tail 各占用一个空间作为哨兵
  pool_size_ = size + 2;
  pool_ = reinterpret_cast<T*>(std::calloc(pool_size_, sizeof(T)));
  if (pool_ == nullptr) {
    return false;
  }

  // 使用 placement new 构造每个元素
  for (uint64_t i = 0; i < pool_size_; ++i) {
    new (&(pool_[i])) T();
  }

  wait_strategy_.reset(strategy);
  return true;
}

// =============================================================================
// 计算队列当前大小
// tail_ - head_ - 1: 减去 head 和 tail 各占的哨兵位
// =============================================================================
template <typename T>
inline uint64_t BoundedQueue<T>::Size() {
  return tail_.load(std::memory_order_acquire) -
         head_.load(std::memory_order_acquire) - 1;
}

// =============================================================================
// 判断队列是否为空
// =============================================================================
template <typename T>
inline bool BoundedQueue<T>::Empty() {
  return Size() == 0;
}

// =============================================================================
// 将全局编号映射到 pool 数组索引
// 使用减法替代取模运算，速度更快
// =============================================================================
template <typename T>
inline uint64_t BoundedQueue<T>::GetIndex(uint64_t num) {
  return num - (num / pool_size_) * pool_size_;
}

// =============================================================================
// 设置等待策略
// =============================================================================
template <typename T>
inline void BoundedQueue<T>::SetWaitStrategy(WaitStrategy* strategy) {
  wait_strategy_.reset(strategy);
}

// =============================================================================
// 中断所有阻塞等待
// =============================================================================
template <typename T>
inline void BoundedQueue<T>::BreakAllWait() {
  break_all_wait_ = true;
  if (wait_strategy_) {
    wait_strategy_->BreakAllWait();
  }
}

}  // namespace minicyber

#endif  // MINICYBER_BASE_BOUNDED_QUEUE_H_
