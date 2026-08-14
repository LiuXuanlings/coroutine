#ifndef MINICYBER_TIME_RATE_H_
#define MINICYBER_TIME_RATE_H_

#include <cstdint>

#include "minicyber/time/duration.h"
#include "minicyber/time/time.h"

namespace minicyber {
namespace time {

class Rate {
 public:
  explicit Rate(double frequency);
  explicit Rate(uint64_t nanoseconds);
  explicit Rate(const Duration& duration);

  void Sleep();
  void Reset();
  Duration CycleTime() const;
  Duration ExpectedCycleTime() const { return expected_cycle_time_; }

 private:
  Time start_;
  Duration expected_cycle_time_;
  Duration actual_cycle_time_;
};

}  // namespace time
}  // namespace minicyber

#endif  // MINICYBER_TIME_RATE_H_
