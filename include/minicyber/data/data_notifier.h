#ifndef MINICYBER_DATA_DATA_NOTIFIER_H_
#define MINICYBER_DATA_DATA_NOTIFIER_H_

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace minicyber {
namespace data {

struct Notifier {
  void SetCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
  }

  bool Invoke() {
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!callback_) {
        return false;
      }
      ++active_callbacks_;
      callback = callback_;
    }
    const Notifier* previous = active_notifier_;
    active_notifier_ = this;
    try {
      callback();
    } catch (...) {
      active_notifier_ = previous;
      std::lock_guard<std::mutex> lock(mutex_);
      --active_callbacks_;
      callbacks_finished_.notify_all();
      throw;
    }
    active_notifier_ = previous;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --active_callbacks_;
    }
    callbacks_finished_.notify_all();
    return true;
  }

  void Deactivate() {
    std::unique_lock<std::mutex> lock(mutex_);
    callback_ = nullptr;
    if (active_notifier_ == this) {
      return;
    }
    callbacks_finished_.wait(lock,
                             [this]() { return active_callbacks_ == 0; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable callbacks_finished_;
  std::function<void()> callback_;
  size_t active_callbacks_ = 0;
  static thread_local const Notifier* active_notifier_;
};

inline thread_local const Notifier* Notifier::active_notifier_ = nullptr;

// Routes data-arrival wakeups to the right coroutine(s). Each channel_id maps
// to a vector of Notifier callbacks. When a writer publishes data, it calls
// Notify(channel_id); every callback registered on that channel fires.
// Notify takes a shared_ptr snapshot under a short map lock, then invokes
// callbacks outside the lock. Removal deactivates the notifier only after it
// has left the map, and waits for in-flight callbacks before returning.
class DataNotifier {
 public:
  using NotifyVector = std::vector<std::shared_ptr<Notifier>>;

  static DataNotifier* Instance() {
    static DataNotifier inst;
    return &inst;
  }

  void AddNotifier(uint64_t channel_id,
                   const std::shared_ptr<Notifier>& notifier) {
    std::lock_guard<std::mutex> lock(notifiers_mtx_);
    notifiers_[channel_id].emplace_back(notifier);
  }

  bool RemoveNotifier(uint64_t channel_id,
                      const std::shared_ptr<Notifier>& notifier) {
    bool removed = false;
    {
      std::lock_guard<std::mutex> lock(notifiers_mtx_);
      auto channel = notifiers_.find(channel_id);
      if (channel == notifiers_.end()) {
        return false;
      }
      auto& callbacks = channel->second;
      callbacks.erase(std::remove_if(callbacks.begin(), callbacks.end(),
                                     [&notifier, &removed](
                                         const std::shared_ptr<Notifier>& item) {
                                       if (item == notifier) {
                                         removed = true;
                                         return true;
                                       }
                                       return false;
                                     }),
                      callbacks.end());
      if (callbacks.empty()) {
        notifiers_.erase(channel);
      }
    }
    if (removed && notifier) {
      notifier->Deactivate();
    }
    return removed;
  }

  bool Notify(uint64_t channel_id) {
    NotifyVector snapshot;
    {
      std::lock_guard<std::mutex> lock(notifiers_mtx_);
      auto channel = notifiers_.find(channel_id);
      if (channel == notifiers_.end()) {
        return false;
      }
      snapshot = channel->second;
    }
    for (auto& notifier : snapshot) {
      if (notifier) {
        notifier->Invoke();
      }
    }
    return true;
  }

 private:
  DataNotifier() = default;
  ~DataNotifier() = default;
  DataNotifier(const DataNotifier&) = delete;
  DataNotifier& operator=(const DataNotifier&) = delete;

  std::mutex notifiers_mtx_;
  std::unordered_map<uint64_t, NotifyVector> notifiers_;
};

}  // namespace data
}  // namespace minicyber

#endif  // MINICYBER_DATA_DATA_NOTIFIER_H_
