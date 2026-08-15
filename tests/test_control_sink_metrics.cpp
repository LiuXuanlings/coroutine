#include <gtest/gtest.h>

#include "demo/autodrive/metrics.h"

TEST(ControlSinkMetricsTest, DisabledMetricsNeverStoresLatencySamples) {
  minicyber::autodrive::Metrics metrics;
  metrics.RecordMeasurement(1, 100, 140, false);
  metrics.RecordMeasurement(2, 200, 250, false);

  EXPECT_EQ(metrics.received, 2u);
  EXPECT_TRUE(metrics.latency_ns.empty());
}

TEST(ControlSinkMetricsTest, EnabledMetricsStoresOnlyValidLatencySamples) {
  minicyber::autodrive::Metrics metrics;
  metrics.RecordMeasurement(1, 100, 140, true);
  metrics.RecordMeasurement(2, 300, 250, true);

  ASSERT_EQ(metrics.latency_ns.size(), 1u);
  EXPECT_EQ(metrics.latency_ns.front(), 40u);
}
