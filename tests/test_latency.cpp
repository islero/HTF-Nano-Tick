/**
 * @file test_latency.cpp
 * @brief Unit tests for latency statistics helpers.
 */

#include <gtest/gtest.h>
#include <core/Timestamp.hpp>

using namespace hft;

TEST(LatencyHistogramTest, PercentileDoesNotExceedRecordedMax) {
    DefaultLatencyHistogram histogram;

    histogram.record(25);
    histogram.record(40);

    EXPECT_LE(histogram.p50(), histogram.max());
    EXPECT_LE(histogram.p99(), histogram.max());
}
