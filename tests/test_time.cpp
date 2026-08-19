#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "minicyber/croutine/croutine.h"
#include "minicyber/time/rate.h"

namespace minicyber {
namespace time {
namespace {

TEST(DurationTest, ConvertsAndCalculatesNanoseconds) {
  const Duration duration(1, 250000000);

  EXPECT_EQ(duration.ToNanosecond(), 1250000000);
  EXPECT_DOUBLE_EQ(duration.ToSecond(), 1.25);
  EXPECT_EQ((duration + Duration(500000000)).ToNanosecond(), 1750000000);
  EXPECT_EQ((duration * 2.0).ToNanosecond(), 2500000000);
}

TEST(TimeTest, MonoTimeAndSleepUntilAdvance) {
  const Time start = Time::MonoTime();
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  EXPECT_GT(Time::MonoTime(), start);

  const Time before_sleep = Time::MonoTime();
  Time::SleepUntil(Time::Now() + Duration(2000000));
  EXPECT_GE((Time::MonoTime() - before_sleep).ToNanosecond(), 1000000);
}

TEST(RateTest, RecordsOverrunAndResetsSchedule) {
  Rate rate(100.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  rate.Sleep();

  EXPECT_GT(rate.CycleTime(), rate.ExpectedCycleTime());

  rate.Reset();
  const Time start = Time::MonoTime();
  rate.Sleep();
  EXPECT_GE((Time::MonoTime() - start).ToNanosecond(), 5000000);
}

}  // namespace
}  // namespace time
}  // namespace minicyber
