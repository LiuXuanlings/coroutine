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
  if (shutdown_.load()) return false;
  if (indicator_ != nullptr) return true;  // 已初始化
  if (!OpenOrCreate()) {
    shutdown_.store(true);
    return false;
  }
  next_seq_ = indicator_->next_seq.load();
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
  if (managed_shm_ == reinterpret_cast<void*>(-1)) {
    ::shmctl(shmid, IPC_RMID, 0);
    managed_shm_ = nullptr;
    return false;
  }

  // placement-new 构造 Indicator（含原子初值 0）
  indicator_ = new (managed_shm_) Indicator();
  return true;
}

bool ConditionNotifier::OpenOnly() {
  int shmid = ::shmget(key_, 0, 0644);
  if (shmid == -1) return false;

  managed_shm_ = ::shmat(shmid, nullptr, 0);
  if (managed_shm_ == reinterpret_cast<void*>(-1)) {
    managed_shm_ = nullptr;
    return false;
  }
  indicator_ = reinterpret_cast<Indicator*>(managed_shm_);
  return indicator_ != nullptr;
}

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
  if (shutdown_.load() || indicator_ == nullptr) return false;
  uint64_t seq = indicator_->next_seq.fetch_add(1);
  uint64_t idx = seq % kBufLength;
  indicator_->infos[idx] = info;
  indicator_->seqs[idx] = seq;
  return true;
}

bool ConditionNotifier::Listen(int timeout_ms, ReadableInfo* info) {
  if (info == nullptr || shutdown_.load() || indicator_ == nullptr) return false;

  // timeout_ms = -1 视为永久；这里用一个很大的 int 上限近似
  int64_t remaining_us = (timeout_ms < 0) ? INT64_MAX : (int64_t)timeout_ms * 1000;

  while (!shutdown_.load()) {
    uint64_t seq = indicator_->next_seq.load();
    if (seq != next_seq_) {
      auto idx = next_seq_ % kBufLength;
      uint64_t actual_seq = indicator_->seqs[idx];
      if (actual_seq >= next_seq_) {
        next_seq_ = actual_seq;
        *info = indicator_->infos[idx];
        ++next_seq_;
        return true;
      }
      // 槽位正在被写，跳过本次，继续轮询
    }

    if (remaining_us <= 0) return false;
    int64_t sleep_us = (remaining_us < 50) ? remaining_us : 50;
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
    remaining_us -= sleep_us;
  }
  return false;
}

void ConditionNotifier::Shutdown() {
  if (shutdown_.exchange(true)) return;
  // 与 CyberRT 一致：留一点时间让对端最后读一次
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  Reset();
}

}  // namespace transport
}  // namespace minicyber