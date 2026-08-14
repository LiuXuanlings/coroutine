#ifndef MINICYBER_DATA_DATA_VISITOR_BASE_H_
#define MINICYBER_DATA_DATA_VISITOR_BASE_H_

#include <functional>
#include <memory>

#include "minicyber/data/data_notifier.h"

namespace minicyber {
namespace data {

// DataVisitorBase 持有 DataNotifier 调用的唤醒回调。RoutineFactory 把此基类
// 交给调度层，调度层再绑定任务的 NotifyTask；回调只唤醒 DATA_WAIT 协程，
// 不得在发布线程直接执行 Proc。对应 CyberRT data::DataVisitorBase。
class DataVisitorBase {
 public:
  DataVisitorBase() : notifier_(std::make_shared<Notifier>()) {}

  // 绑定数据到达唤醒回调。调用方必须先完成任务生命周期注册，再允许发布，
  // 避免关闭后的 notifier 指向已释放的 Scheduler/Component。
  void RegisterNotifyCallback(std::function<void()>&& callback) {
    notifier_->SetCallback(std::move(callback));
  }

 protected:
  DataVisitorBase(const DataVisitorBase&) = delete;
  DataVisitorBase& operator=(const DataVisitorBase&) = delete;

  // 下一个待消费消息的绝对游标；每次成功 TryFetch/Fusion 后恰好前进一次。
  uint64_t next_msg_index_ = 0;
  DataNotifier* data_notifier_ = DataNotifier::Instance();
  std::shared_ptr<Notifier> notifier_;
};

}  // namespace data
}  // namespace minicyber

#endif  // MINICYBER_DATA_DATA_VISITOR_BASE_H_
