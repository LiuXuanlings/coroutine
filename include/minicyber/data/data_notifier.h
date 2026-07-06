#ifndef MINICYBER_DATA_DATA_NOTIFIER_H_
#define MINICYBER_DATA_DATA_NOTIFIER_H_

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "minicyber/base/atomic_hash_map.h"

namespace minicyber {
namespace data {

struct Notifier {
  std::function<void()> callback;
};

// Routes data-arrival wakeups to the right coroutine(s). Each channel_id maps
// to a vector of Notifier callbacks. When a writer publishes data, it calls
// Notify(channel_id); every callback registered on that channel fires.
// The hot path (Notify) is lock-free via AtomicHashMap; only AddNotifier
// (registration, cold path) takes a mutex to protect the vector append.
class DataNotifier {
 public:
  using NotifyVector = std::vector<std::shared_ptr<Notifier>>;

  static DataNotifier* Instance() {
    static DataNotifier inst;
    return &inst;
  }

  void AddNotifier(uint64_t channel_id,
                   const std::shared_ptr<Notifier>& notifier) {
    std::lock_guard<std::mutex> lock(notifies_map_mutex_);
    NotifyVector* notifies = nullptr;
    if (notifies_map_.Get(channel_id, &notifies)) {
      notifies->emplace_back(notifier);
    } else {
      NotifyVector new_notify = {notifier};
      notifies_map_.Set(channel_id, new_notify);
    }
  }

  bool Notify(uint64_t channel_id) {
    NotifyVector* notifies = nullptr;
    if (notifies_map_.Get(channel_id, &notifies)) {
      for (auto& notifier : *notifies) {
        if (notifier && notifier->callback) {
          notifier->callback();
        }
      }
      return true;
    }
    return false;
  }

 private:
  DataNotifier() = default;
  ~DataNotifier() = default;
  DataNotifier(const DataNotifier&) = delete;
  DataNotifier& operator=(const DataNotifier&) = delete;

  std::mutex notifies_map_mutex_;
  AtomicHashMap<uint64_t, NotifyVector, 128> notifies_map_;
};

}  // namespace data
}  // namespace minicyber

#endif  // MINICYBER_DATA_DATA_NOTIFIER_H_