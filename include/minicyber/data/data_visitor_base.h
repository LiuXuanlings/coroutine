#ifndef MINICYBER_DATA_DATA_VISITOR_BASE_H_
#define MINICYBER_DATA_DATA_VISITOR_BASE_H_

#include <functional>
#include <memory>

#include "minicyber/data/data_notifier.h"

namespace minicyber {
namespace data {

// Common base for DataVisitor and DataFusion. Owns the Notifier that
// DataNotifier will fire when data arrives on the visitor's channel(s).
// The Notifier's callback is bound by the Scheduler/Node layer (typically
// `rt->SetUpdateFlag()` + re-enqueue) so a waiting coroutine wakes up.
class DataVisitorBase {
 public:
  DataVisitorBase() : notifier_(std::make_shared<Notifier>()) {}

  // Bind the wake-up callback. Invoked by DataNotifier::Notify(channel_id)
  // when a writer publishes data on a channel this visitor subscribes to.
  void RegisterNotifyCallback(std::function<void()>&& callback) {
    notifier_->SetCallback(std::move(callback));
  }

 protected:
  DataVisitorBase(const DataVisitorBase&) = delete;
  DataVisitorBase& operator=(const DataVisitorBase&) = delete;

  // Absolute index of the next message to consume. Bumped after each
  // successful TryFetch / Fusion so the visitor tracks its own read cursor.
  uint64_t next_msg_index_ = 0;
  DataNotifier* data_notifier_ = DataNotifier::Instance();
  std::shared_ptr<Notifier> notifier_;
};

}  // namespace data
}  // namespace minicyber

#endif  // MINICYBER_DATA_DATA_VISITOR_BASE_H_
