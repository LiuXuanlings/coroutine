#include "minicyber/time/time.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>
#include <thread>

namespace minicyber {
namespace time {

const Time Time::MAX = Time(std::numeric_limits<uint64_t>::max());
const Time Time::MIN = Time(0);

Time::Time(uint64_t nanoseconds) : nanoseconds_(nanoseconds) {}

Time::Time(int nanoseconds) : nanoseconds_(static_cast<uint64_t>(nanoseconds)) {}

Time::Time(double seconds)
    : nanoseconds_(static_cast<uint64_t>(seconds * 1000000000UL)) {}

Time::Time(uint32_t seconds, uint32_t nanoseconds)
    : nanoseconds_(static_cast<uint64_t>(seconds) * 1000000000UL + nanoseconds) {}

Time Time::Now() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return Time(static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()));
}

Time Time::MonoTime() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return Time(static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()));
}

void Time::SleepUntil(const Time& time) {
  std::this_thread::sleep_until(std::chrono::system_clock::time_point(
      std::chrono::nanoseconds(time.ToNanosecond())));
}

double Time::ToSecond() const {
  return static_cast<double>(nanoseconds_) / 1000000000UL;
}

uint64_t Time::ToMicrosecond() const { return nanoseconds_ / 1000UL; }

uint64_t Time::ToNanosecond() const { return nanoseconds_; }

std::string Time::ToString() const {
  const auto nanoseconds = std::chrono::nanoseconds(nanoseconds_);
  const auto time_point = std::chrono::system_clock::time_point(nanoseconds);
  const auto seconds = std::chrono::system_clock::to_time_t(time_point);
  struct tm local_time {};
  if (localtime_r(&seconds, &local_time) == nullptr) {
    return std::to_string(ToSecond());
  }

  std::ostringstream stream;
  stream << std::put_time(&local_time, "%F %T") << '.' << std::setw(9)
         << std::setfill('0') << nanoseconds_ % 1000000000UL;
  return stream.str();
}

bool Time::IsZero() const { return nanoseconds_ == 0; }

Duration Time::operator-(const Time& rhs) const {
  return Duration(static_cast<int64_t>(nanoseconds_ - rhs.nanoseconds_));
}

Time Time::operator+(const Duration& rhs) const {
  return Time(nanoseconds_ + rhs.ToNanosecond());
}

Time Time::operator-(const Duration& rhs) const {
  return Time(nanoseconds_ - rhs.ToNanosecond());
}

Time& Time::operator+=(const Duration& rhs) {
  *this = *this + rhs;
  return *this;
}

Time& Time::operator-=(const Duration& rhs) {
  *this = *this - rhs;
  return *this;
}

bool Time::operator==(const Time& rhs) const {
  return nanoseconds_ == rhs.nanoseconds_;
}

bool Time::operator!=(const Time& rhs) const { return !(*this == rhs); }

bool Time::operator>(const Time& rhs) const { return nanoseconds_ > rhs.nanoseconds_; }

bool Time::operator<(const Time& rhs) const { return nanoseconds_ < rhs.nanoseconds_; }

bool Time::operator>=(const Time& rhs) const { return !(*this < rhs); }

bool Time::operator<=(const Time& rhs) const { return !(*this > rhs); }

std::ostream& operator<<(std::ostream& os, const Time& rhs) {
  os << rhs.ToString();
  return os;
}

}  // namespace time
}  // namespace minicyber
