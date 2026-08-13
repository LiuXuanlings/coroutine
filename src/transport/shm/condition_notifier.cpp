#include "minicyber/transport/shm/condition_notifier.h"

#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace minicyber {
namespace transport {

namespace {
// 与 CyberRT 一致：用一个固定字符串的 hash 作为 SHM key
// 这里用简单 hash 复刻 std::hash<std::string> 的效果（CyberRT 用 common::Hash）
key_t MakeKey() {
  const char* p = "/minicyber/transport/shm/notifier";
  uint64_t h = 0;
  for (const char* c = p; *c; ++c) {
    h = h * 131u + static_cast<uint64_t>(*c);
  }
  return static_cast<key_t>(h & 0x7fffffff);  // key_t 需正数
}
}  // namespace

ConditionNotifier::ConditionNotifier() {
  key_ = MakeKey();
  shm_size_ = sizeof(Indicator);
}

ConditionNotifier::~ConditionNotifier() { Shutdown(); }

bool ConditionNotifier::Init() {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (shutdown_.load(std::memory_order_acquire)) return false;
  if (indicator_ != nullptr) return true;
  if (!OpenOrCreate()) {
    shutdown_.store(true);
    return false;
  }
  next_seq_ = indicator_->next_seq.load(std::memory_order_acquire);
  return true;
}

bool ConditionNotifier::OpenOrCreate() {
  int retry = 0;
  int shmid = 0;
  while (retry < 2) {
    shmid = ::shmget(key_, shm_size_, 0644 | IPC_CREAT | IPC_EXCL);
    if (shmid != -1) break;

    if (EINVAL == errno) {
      // 大小不匹配，重建
      Reset();
      Remove();
      ++retry;
    } else if (EEXIST == errno) {
      return OpenOnly();
    } else {
      return false;
    }
  }
  if (shmid == -1) return false;

  managed_shm_ = ::shmat(shmid, nullptr, 0);
  if (managed_shm_ == reinterpret_cast<void*>(-1)) {// when shmat fails, it returns (void*)-1
    ::shmctl(shmid, IPC_RMID, 0);
    managed_shm_ = nullptr;
    return false;
  }

  // placement-new 构造 Indicator（含原子初值 0）
  indicator_ = new (managed_shm_) Indicator();
  return true;
}

bool ConditionNotifier::OpenOnly() {
  int shmid = ::shmget(key_, shm_size_, 0644);
  if (shmid == -1) return false;

  managed_shm_ = ::shmat(shmid, nullptr, 0);// attach the shared memory segment to the process's address space
  if (managed_shm_ == reinterpret_cast<void*>(-1)) {
    managed_shm_ = nullptr;
    return false;
  }
  indicator_ = reinterpret_cast<Indicator*>(managed_shm_);
  return indicator_ != nullptr;
}

//difference between Remove() and Reset():
// Remove() removes the shared memory segment from the system, 
// while Reset() only detaches the shared memory segment from the process's address space. 
bool ConditionNotifier::Remove() {
  int shmid = ::shmget(key_, 0, 0644);
  if (shmid == -1) return false;
  return ::shmctl(shmid, IPC_RMID, 0) == 0; // Remove identifier
}

void ConditionNotifier::Reset() {
  indicator_ = nullptr;
  if (managed_shm_ != nullptr) {
    ::shmdt(managed_shm_);
    managed_shm_ = nullptr;
  }
}

bool ConditionNotifier::Notify(const ReadableInfo& info) {
  if (!BeginOperation()) return false;
  struct OperationGuard {
    ConditionNotifier* notifier;
    ~OperationGuard() { notifier->EndOperation(); }
  } guard{this};

  while (indicator_->writer_lock.test_and_set(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  struct WriterGuard {
    std::atomic_flag* lock;
    ~WriterGuard() { lock->clear(std::memory_order_release); }
  } writer_guard{&indicator_->writer_lock};

  const uint64_t seq = indicator_->next_seq.load(std::memory_order_relaxed);
  uint64_t idx = seq % kBufLength;
  indicator_->infos[idx] = info;
  indicator_->seqs[idx].store(seq, std::memory_order_release);
  indicator_->next_seq.store(seq + 1, std::memory_order_release);
  return true;
}

bool ConditionNotifier::Listen(int timeout_ms, ReadableInfo* info) {
  if (info == nullptr || !BeginOperation()) return false;
  struct OperationGuard {
    ConditionNotifier* notifier;
    ~OperationGuard() { notifier->EndOperation(); }
  } guard{this};

  const auto deadline = timeout_ms < 0
                            ? std::chrono::steady_clock::time_point::max()
                            : std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(timeout_ms);

  while (!shutdown_.load(std::memory_order_acquire)) {
    const uint64_t seq = indicator_->next_seq.load(std::memory_order_acquire);
    if (seq != next_seq_) {
      // 计算当前期望序列号对应的环形槽位，通过槽内真实序列号校验数据有效性
      // 分三种场景处理：
      // 1. actual_seq == next_seq_：正常顺序消费，读取数据后消费进度+1
      // 2. actual_seq >  next_seq_：消费者落后过多，历史数据已被新消息覆盖
      //    触发 fast-forward 快进：直接对齐到槽位当前有效序列号，跳过全部丢失的历史消息
      // 3. actual_seq < next_seq_：读到了未提交或已被重写的槽位，等待
      //    下一个已发布序列或由 fast-forward 处理覆盖窗口。
      const auto idx = next_seq_ % kBufLength;
      const uint64_t actual_seq =
          indicator_->seqs[idx].load(std::memory_order_acquire);
      if (actual_seq == next_seq_) {
        *info = indicator_->infos[idx];
        ++next_seq_;
        return true;
      }
      if (actual_seq != kUnpublishedSeq && actual_seq > next_seq_) {
        next_seq_ = actual_seq;
        *info = indicator_->infos[idx];
        ++next_seq_;
        return true;
      }
    }

    if (timeout_ms == 0 || std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  return false;
}

void ConditionNotifier::Shutdown() {
  {
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    if (shutdown_.exchange(true, std::memory_order_acq_rel)) return;
    operations_finished_.wait(lock, [this]() { return active_operations_ == 0; });
  }
  Reset();
}

bool ConditionNotifier::BeginOperation() {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (shutdown_.load(std::memory_order_acquire) || indicator_ == nullptr) {
    return false;
  }
  ++active_operations_;
  return true;
}

void ConditionNotifier::EndOperation() {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  --active_operations_;
  if (active_operations_ == 0) {
    operations_finished_.notify_all();
  }
}

}  // namespace transport
}  // namespace minicyber
