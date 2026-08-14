#include "minicyber/time/duration.h"

#include <chrono>
#include <iomanip>
#include <ostream>
#include <thread>

namespace minicyber {
namespace time {

Duration::Duration(int64_t nanoseconds) : nanoseconds_(nanoseconds) {}

Duration::Duration(int nanoseconds) : nanoseconds_(nanoseconds) {}

Duration::Duration(double seconds)
    : nanoseconds_(static_cast<int64_t>(seconds * 1000000000UL)) {}

Duration::Duration(uint32_t seconds, uint32_t nanoseconds)
    : nanoseconds_(static_cast<int64_t>(seconds) * 1000000000LL + nanoseconds) {}

double Duration::ToSecond() const {
  return static_cast<double>(nanoseconds_) / 1000000000UL;
}

int64_t Duration::ToNanosecond() const { return nanoseconds_; }

bool Duration::IsZero() const { return nanoseconds_ == 0; }

void Duration::Sleep() const {
  std::this_thread::sleep_for(std::chrono::nanoseconds(nanoseconds_));
}

Duration Duration::operator+(const Duration& rhs) const {
  return Duration(nanoseconds_ + rhs.nanoseconds_);
}

Duration Duration::operator-(const Duration& rhs) const {
  return Duration(nanoseconds_ - rhs.nanoseconds_);
}

Duration Duration::operator-() const { return Duration(-nanoseconds_); }

Duration Duration::operator*(double scale) const {
  return Duration(static_cast<int64_t>(static_cast<double>(nanoseconds_) * scale));
}

Duration& Duration::operator+=(const Duration& rhs) {
  *this = *this + rhs;
  return *this;
}

Duration& Duration::operator-=(const Duration& rhs) {
  *this = *this - rhs;
  return *this;
}

Duration& Duration::operator*=(double scale) {
  *this = *this * scale;
  return *this;
}

bool Duration::operator==(const Duration& rhs) const {
  return nanoseconds_ == rhs.nanoseconds_;
}

bool Duration::operator!=(const Duration& rhs) const { return !(*this == rhs); }

bool Duration::operator>(const Duration& rhs) const {
  return nanoseconds_ > rhs.nanoseconds_;
}

bool Duration::operator<(const Duration& rhs) const {
  return nanoseconds_ < rhs.nanoseconds_;
}

bool Duration::operator>=(const Duration& rhs) const { return !(*this < rhs); }

bool Duration::operator<=(const Duration& rhs) const { return !(*this > rhs); }

std::ostream& operator<<(std::ostream& os, const Duration& rhs) {
  const auto flags = os.flags();
  os << std::fixed << std::setprecision(9) << rhs.ToSecond() << 's';
  os.flags(flags);
  return os;
}

}  // namespace time
}  // namespace minicyber
