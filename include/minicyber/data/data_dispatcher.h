#ifndef MINICYBER_DATA_DATA_DISPATCHER_H_
#define MINICYBER_DATA_DATA_DISPATCHER_H_

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "minicyber/data/channel_buffer.h"
#include "minicyber/data/data_notifier.h"

namespace minicyber {
namespace data {

// Routes a published message to every ChannelBuffer registered on a channel,
// then fires DataNotifier so waiting coroutines wake up.
//
// Buffer ownership is weak: the dispatcher holds weak_ptr to each CacheBuffer,
// so destroying a ChannelBuffer/DataVisitor simply leaves a dead entry that
// gets skipped on the next Dispatch.
//
// Lock strategy (Step 14, shared_mutex version — to be replaced by
// AtomicHashMap in Step 26):
//   - AddBuffer: exclusive write lock on the map.
//   - Dispatch:  shared read lock on the map; each buffer is filled under
//                its own mutex. The map lock is released BEFORE Notify fires,
//                so a callback that re-enters the dispatcher cannot deadlock.
template <typename T>
class DataDispatcher {
 public:
  using BufferType = CacheBuffer<std::shared_ptr<T>>;
  using BufferVector = std::vector<std::weak_ptr<BufferType>>;

  static DataDispatcher<T>* Instance() {
    static DataDispatcher<T> inst;
    return &inst;
  }

  void AddBuffer(const ChannelBuffer<T>& channel_buffer) {
    std::unique_lock<std::shared_mutex> lock(buffers_map_mutex_);
    // 取临时强引用，保证插入过程中缓存对象不会被并发销毁
    auto buffer = channel_buffer.Buffer();
    auto it = buffers_map_.find(channel_buffer.channel_id());
    if (it != buffers_map_.end()) {
      // std::move为语义标记：标识局部变量后续不再使用
      // 容器存weak_ptr，无实际shared_ptr所有权转移，强引用计数不变
      it->second.emplace_back(std::move(buffer));
    } else {
      buffers_map_.emplace(channel_buffer.channel_id(),
                           BufferVector{std::move(buffer)});
    }
    // 函数退出后局部buffer析构，强引用计数-1，缓存所有权始终归ChannelBuffer持有
  }

  bool Dispatch(uint64_t channel_id, const std::shared_ptr<T>& msg) {
    // Snapshot the buffer list under a shared lock, then release the map lock
    // before filling/notify so callbacks can safely re-enter the dispatcher.
    // 分发策略：共享锁下拷贝当前通道的订阅缓存列表为本地快照，随即释放map锁
    // 快照设计合理性：
    // 1. 仅拷贝 vector<weak_ptr> 指针集合，拷贝成本极低，性能开销可忽略
    // 2. 订阅增删属于低频操作，快照与实时 map 的短暂不一致仅影响本次分发，下一次即可同步，属于可接受的最终一致
    // 提前释放锁的核心原因：
    // 1. shared_mutex 不支持递归加锁，回调重入分发器无论读写均属于未定义行为，易触发死锁
    // 2. 若回调内调用 AddBuffer 申请写锁，会因当前线程自身持有读锁而永久等待
    // 3. 缩小锁粒度，写缓存、发通知全程不占用map锁，大幅提升并发分发吞吐量
    BufferVector snapshot;
    {
      std::shared_lock<std::shared_mutex> lock(buffers_map_mutex_);
      auto it = buffers_map_.find(channel_id);
      if (it == buffers_map_.end()) {
        return false;
      }
      snapshot = it->second;
    }

    for (auto& buffer_wptr : snapshot) {
      // lock() 同时实现两点：
      // 1. 获取临时强引用，保证 Fill 写入全程缓存对象不会被并发销毁
      // 2. 自动过滤失效项：ChannelBuffer 销毁后强引用归零，weak_ptr 失效，lock() 返回空指针则自动跳过，无需单独提供注销接口
      // 失效死项暂留 vector 不主动清理，订阅增删为低频操作，属于可接受的低成本权衡
      if (auto buffer = buffer_wptr.lock()) {
        std::lock_guard<std::mutex> lg(buffer->Mutex());
        buffer->Fill(msg);
      }
    }
    return notifier_->Notify(channel_id);
  }

 private:
  DataDispatcher() = default;
  ~DataDispatcher() = default;
  DataDispatcher(const DataDispatcher&) = delete;
  DataDispatcher& operator=(const DataDispatcher&) = delete;

  DataNotifier* notifier_ = DataNotifier::Instance();
  mutable std::shared_mutex buffers_map_mutex_;
  std::unordered_map<uint64_t, BufferVector> buffers_map_;
};

}  // namespace data
}  // namespace minicyber

#endif  // MINICYBER_DATA_DATA_DISPATCHER_H_