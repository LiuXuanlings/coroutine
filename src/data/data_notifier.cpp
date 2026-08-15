#include "minicyber/data/data_notifier.h"

namespace minicyber {
namespace data {

DataNotifier* DataNotifier::Instance() {
  // 核心运行时在进程退出时统一销毁该总线；插件只借用，不取得所有权。
  static DataNotifier instance;
  return &instance;
}

}  // namespace data
}  // namespace minicyber
