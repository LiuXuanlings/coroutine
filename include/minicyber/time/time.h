#ifndef MINICYBER_TIME_TIME_H_
#define MINICYBER_TIME_TIME_H_

#include <chrono>
#include <cstdint>
#include <string>

namespace minicyber {

// =============================================================================
// Duration：纳秒级时间间隔（对齐 CyberRT Duration）
// =============================================================================
class Duration {
 public:
  Duration() = default;
  explicit Duration(int64_t nanoseconds) : nanoseconds_(nanoseconds) {}

  double ToSecond() const { return static_cast<double>(nanoseconds_) / 1e9; }
  int64_t ToNanosecond() const { return nanoseconds_; }
  int64_t ToMicrosecond() const { return nanoseconds_ / 1000; }
  bool IsZero() const { return nanoseconds_ == 0; }

 private:
  int64_t nanoseconds_ = 0;
};

// =============================================================================
// Time：纳秒级时间戳（对齐 CyberRT Time）
//
// 用法：
//   auto t1 = Time::MonoTime();
//   do_work();
//   auto t2 = Time::MonoTime();
//   double us = (t2 - t1).ToMicrosecond();
// =============================================================================
class Time {
 public:
  Time() = default;
  explicit Time(uint64_t nanoseconds) : nanoseconds_(nanoseconds) {}

  // 壁钟时间（UTC），适用于日志和显示
  static Time Now() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return Time(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()));
  }

  // 单调递增时间，适用于性能测量（不受系统时间跳变影响）
  static Time MonoTime() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return Time(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()));
  }

  double ToSecond() const { return static_cast<double>(nanoseconds_) / 1e9; }
  uint64_t ToMicrosecond() const { return nanoseconds_ / 1000; }
  uint64_t ToNanosecond() const { return nanoseconds_; }

  Duration operator-(const Time& rhs) const {
    return Duration(static_cast<int64_t>(nanoseconds_ - rhs.nanoseconds_));
  }

  std::string ToString() const {
    auto sec = nanoseconds_ / 1000000000UL;
    auto nsec = nanoseconds_ % 1000000000UL;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%lu.%09lu",
                     static_cast<unsigned long>(sec),
                     static_cast<unsigned long>(nsec));
    return std::string(buf, n);
  }

 private:
  uint64_t nanoseconds_ = 0;
};

}  // namespace minicyber

#endif  // MINICYBER_TIME_TIME_H_
