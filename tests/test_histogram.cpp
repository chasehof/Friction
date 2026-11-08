///
/// \file test_histogram.cpp
///
/// Tests for the distribution behind every reported number.
///
/// The bucket arithmetic is the part most likely to be quietly wrong: an index
/// that runs off the end of the slot array corrupts memory, and one that is off
/// by a bucket still produces plausible looking percentages. So the bounds are
/// checked against the values that produced them, exhaustively, rather than by
/// eyeballing a percentile.
///
#include "harness.hpp"

#include <friction/friction.hpp>
#include <array>
#include <numeric>
#include <random>
#include <vector>

using friction::Histogram;

FRICTION_TEST(histogram, starts_empty) {
    const Histogram histogram;
    CHECK_EQ(histogram.count(), 0ull);
    CHECK_EQ(histogram.sum(), 0ull);
    CHECK_EQ(histogram.min(), 0ull);
    CHECK_EQ(histogram.max(), 0ull);
    CHECK_NEAR(histogram.mean(), 0.0, 1.0e-12);
    CHECK_NEAR(histogram.variance(), 0.0, 1.0e-12);
    CHECK_EQ(histogram.median(), 0ull);
    CHECK_EQ(histogram.p99(), 0ull);
}

FRICTION_TEST(histogram, tracks_the_exact_totals) {
    Histogram histogram;
    for (std::uint64_t value = 1; value <= 1000; ++value) {
        histogram.record(value);
    }

    CHECK_EQ(histogram.count(), 1000ull);
    CHECK_EQ(histogram.sum(), 500'500ull);
    CHECK_EQ(histogram.min(), 1ull);
    CHECK_EQ(histogram.max(), 1000ull);
    CHECK_NEAR(histogram.mean(), 500.5, 1.0e-9);
}

FRICTION_TEST(histogram, every_bucket_contains_its_own_value) {
    // The core invariant: whatever a value rounds to, the bucket it landed in
    // has to bracket that value, or every percentile is wrong in a direction
    // nobody can predict.
    Histogram histogram;

    std::vector<std::uint64_t> probes;
    probes.reserve(20'000);
    for (std::uint64_t value = 0; value < 5000; ++value) {
        probes.push_back(value);
    }
    for (int exponent = 0; exponent < 10; ++exponent) {
        const auto base = std::uint64_t{1} << (exponent * 3);
        for (std::uint64_t offset = 0; offset < 2000; ++offset) {
            probes.push_back(base + offset);
        }
    }

    for (const auto value : probes) {
        const auto [lowest, highest] = histogram.bucketBoundsFor(value);
        if (!(lowest <= value && value <= highest)) {
            CHECK_EQ(std::to_string(value) + " not inside [" + std::to_string(lowest) + ", " +
                        std::to_string(highest) + "]",
                     std::string("bucket bounds"));
        }
    }
}

FRICTION_TEST(histogram, bucket_width_stays_within_the_configured_bound) {
    for (const std::uint8_t digits : std::array<std::uint8_t, 3>{1, 2, 3}) {
        Histogram histogram(Histogram::kDefaultHighestTrackableValue, digits);
        const double bound = histogram.relativeErrorBound();

        std::mt19937_64 random{12345};
        std::uniform_int_distribution<std::uint64_t> distribution{1, 1'000'000'000};

        for (int sample = 0; sample < 20'000; ++sample) {
            const auto value = distribution(random);
            const auto [lowest, highest] = histogram.bucketBoundsFor(value);
            const double relative = static_cast<double>(highest - lowest) / static_cast<double>(highest);
            if (relative > bound + 1.0e-12) {
                CHECK_NEAR(relative, bound, 1.0e-9);
            }
        }
    }
}

FRICTION_TEST(histogram, more_digits_give_a_tighter_bound) {
    Histogram coarse(Histogram::kDefaultHighestTrackableValue, 1);
    Histogram fine(Histogram::kDefaultHighestTrackableValue, 3);
    CHECK(coarse.relativeErrorBound() > fine.relativeErrorBound());
    CHECK(fine.memoryUsage() > coarse.memoryUsage() * 8);
}

FRICTION_TEST(histogram, percentiles_land_where_they_should) {
    Histogram histogram;
    // 990 samples at 100, ten at 10,000: p50 and p90 sit in the bulk, p99 lands
    // just inside the tail.
    for (int index = 0; index < 990; ++index) {
        histogram.record(100);
    }
    for (int index = 0; index < 10; ++index) {
        histogram.record(10'000);
    }

    const double bound = histogram.relativeErrorBound();
    CHECK_NEAR(static_cast<double>(histogram.median()), 100.0, 100.0 * bound);
    CHECK_NEAR(static_cast<double>(histogram.p90()), 100.0, 100.0 * bound);
    CHECK_NEAR(static_cast<double>(histogram.p99()), 10'000.0, 10'000.0 * bound);
    CHECK_EQ(histogram.max(), 10'000ull);
    CHECK_EQ(histogram.min(), 100ull);
}

FRICTION_TEST(histogram, lowest_percentile_is_the_minimum) {
    Histogram histogram;
    histogram.record(500);
    histogram.record(7);
    histogram.record(90'000);
    CHECK_EQ(histogram.valueAtPercentile(0.0), 7ull);
}

FRICTION_TEST(histogram, clamps_percentiles_outside_the_range) {
    Histogram histogram;
    for (int index = 0; index < 10; ++index) {
        histogram.record(1000 + static_cast<std::uint64_t>(index));
    }
    CHECK_EQ(histogram.valueAtPercentile(-50.0), histogram.min());
    CHECK_EQ(histogram.valueAtPercentile(100.0), histogram.max());
    CHECK_EQ(histogram.valueAtPercentile(250.0), histogram.max());
}

FRICTION_TEST(histogram, saturates_instead_of_growing) {
    Histogram histogram(100'000, 2);
    CHECK_FALSE(histogram.saturated());

    histogram.record(99'999);
    CHECK_FALSE(histogram.saturated());

    histogram.record(100'000);
    CHECK(histogram.saturated());

    // Absurd inputs must be folded into the top bucket, not index past the end.
    histogram.record(std::uint64_t{1} << 62);
    CHECK_EQ(histogram.count(), 3ull);
    CHECK_EQ(histogram.max(), std::uint64_t{1} << 62);
    CHECK(histogram.p99() >= 100'000);
}

FRICTION_TEST(histogram, merges_like_a_single_distribution) {
    Histogram first;
    Histogram second;
    for (int index = 0; index < 500; ++index) {
        first.record(100);
        second.record(200);
    }

    first.merge(second);
    CHECK_EQ(first.count(), 1000ull);
    CHECK_EQ(first.sum(), 150'000ull);
    CHECK_EQ(first.min(), 100ull);
    CHECK_EQ(first.max(), 200ull);
    // The bulk straddles the seam, so the median lands between the two values.
    const auto median = first.median();
    CHECK(median >= 100 && median <= 200);
}

FRICTION_TEST(histogram, merging_an_empty_histogram_changes_nothing) {
    Histogram histogram;
    histogram.record(42);
    Histogram empty;

    histogram.merge(empty);
    CHECK_EQ(histogram.count(), 1ull);
    CHECK_EQ(histogram.max(), 42ull);

    empty.merge(histogram);
    CHECK_EQ(empty.count(), 1ull);
}

FRICTION_TEST(histogram, reset_keeps_the_allocation) {
    Histogram histogram;
    for (int index = 0; index < 100; ++index) {
        histogram.record(1000);
    }
    const std::size_t before = histogram.memoryUsage();

    histogram.reset();
    CHECK_EQ(histogram.count(), 0ull);
    CHECK_EQ(histogram.min(), 0ull);
    CHECK_EQ(histogram.memoryUsage(), before);
}

FRICTION_TEST(histogram, computes_a_real_standard_deviation) {
    Histogram fine(Histogram::kDefaultHighestTrackableValue, 3);
    for (int index = 0; index < 10'000; ++index) {
        fine.record(1000);
    }
    for (int index = 0; index < 10'000; ++index) {
        fine.record(3000);
    }

    // Half at 1000 and half at 3000: mean 2000, population spread 1000.
    CHECK_NEAR(fine.mean(), 2000.0, 1.0);
    CHECK_NEAR(fine.standardDeviation(), 1000.0, 20.0);
    CHECK_NEAR(fine.variance(), 1.0e6, 2.0e4);
}

FRICTION_TEST(histogram, memory_estimate_matches_reality) {
    const auto estimate =
        Histogram::memoryUsageFor(Histogram::kDefaultHighestTrackableValue, Histogram::kDefaultSignificantFigures);
    const Histogram histogram;
    CHECK_EQ(estimate, histogram.memoryUsage());
    CHECK(estimate > 0);
}
