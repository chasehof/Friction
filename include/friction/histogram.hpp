#pragma once

///
/// \file histogram.hpp
///
/// Bounded log-linear histogram.
///
/// Recording a sample is a shift, a subtract and one increment into a
/// pre-allocated slot, so it costs the same whether the caller has measured ten
/// samples or ten million. Nothing is allocated after construction, which is
/// what makes it safe to keep one per zone per thread.
///
/// The value axis is split into buckets whose width doubles as they climb, and
/// each bucket is subdivided into a fixed number of slots. A bucket at exponent
/// `b` holds the samples whose `b`-th bit shifted down lands on the slot's
/// sub-index, so the slot covers `[subIndex << b, (subIndex + 1) << b)`. Values
/// near the top of the range therefore get the same relative resolution as
/// values near the bottom, which is the property a latency distribution needs
/// and a linear histogram cannot give.
///
///
#include <friction/detail/platform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

namespace friction {

/// Fixed-capacity distribution of unsigned samples with O(1) recording.
class Histogram final {
public:
    /// Digits of relative resolution, 1 through 5.
    ///
    /// Each extra digit roughly quadruples the memory. Latency work is almost
    /// always fine at one, and should only pay for more when it has to.
    static constexpr std::uint8_t kDefaultSignificantFigures = 1;

    /// Default top of the range.
    ///
    /// Samples are unitless. Fed tick deltas this covers just over three seconds
    /// on a 3 GHz counter; fed nanoseconds it covers ten.
    static constexpr std::uint64_t kDefaultHighestTrackableValue = 10'000'000'000ull;

    /// Range and resolution to build a histogram with.
    struct Config {
        std::uint64_t highestTrackableValue = kDefaultHighestTrackableValue;
        std::uint8_t significantFigures = kDefaultSignificantFigures;
    };

    Histogram() noexcept : Histogram(Config{}) {}

    explicit Histogram(const Config& config) noexcept
        : layout_(computeLayout(config.highestTrackableValue, config.significantFigures)),
          slots_(layout_.slotCount, 0) {}

    Histogram(std::uint64_t highestTrackableValue, std::uint8_t significantFigures) noexcept
        : Histogram(Config{highestTrackableValue, significantFigures}) {}

    /// Records one sample, saturating at the configured range rather than growing.
    void record(std::uint64_t value) noexcept {
        ++slots_[slotFor(value)];
        ++count_;
        sum_ += value;
        min_ = FRICTION_UNLIKELY(value < min_) ? value : min_;
        max_ = FRICTION_UNLIKELY(value > max_) ? value : max_;
    }

    /// Folds another histogram in, as when merging per-thread results.
    void merge(const Histogram& other) noexcept {
        if (other.count_ == 0) {
            return;
        }
        if (count_ == 0 || other.min_ < min_) {
            min_ = other.min_;
        }
        if (other.max_ > max_) {
            max_ = other.max_;
        }
        count_ += other.count_;
        sum_ += other.sum_;
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            slots_[index] += other.slots_[index];
        }
    }

    /// Replaces the geometry and drops every sample.
    ///
    /// Used when a session configures the histogram differently from the one a
    /// thread first registered with. The slot vector is reallocated only when the
    /// slot count actually differs, so re-applying the same configuration leaves
    /// the allocation alone.
    void configure(const Config& config) noexcept {
        const Layout next = computeLayout(config.highestTrackableValue, config.significantFigures);
        if (next.slotCount != layout_.slotCount) {
            slots_.assign(next.slotCount, 0);
        }
        layout_ = next;
        reset();
    }

    /// Clears every recorded sample, keeping the allocated slots.
    void reset() noexcept {
        std::fill(slots_.begin(), slots_.end(), 0);
        count_ = 0;
        sum_ = 0;
        min_ = kUnset;
        max_ = 0;
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t sum() const noexcept { return sum_; }
    [[nodiscard]] std::uint64_t min() const noexcept { return count_ == 0 ? 0 : min_; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }

    /// True when samples have reached the top of the configured range, which
    /// means the reported extremes are floors rather than measurements.
    [[nodiscard]] bool saturated() const noexcept { return count_ != 0 && max_ >= layout_.highestTrackableValue; }

    [[nodiscard]] double mean() const noexcept {
        return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
    }

    /// Sample variance, derived from the slots rather than from a second running
    /// sum of squares. The two agree closely until the samples span more than a
    /// few hundred-fold range in magnitude, which a log-linear histogram reports
    /// faithfully and a sum of squares would have already lost.
    [[nodiscard]] double variance() const noexcept {
        if (count_ < 2) {
            return 0.0;
        }
        const double n = static_cast<double>(count_);
        double weightedSquares = 0.0;
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (slots_[index] == 0) {
                continue;
            }
            const auto value = static_cast<double>(highestEquivalentValue(index));
            weightedSquares += static_cast<double>(slots_[index]) * value * value;
        }
        const double spread = weightedSquares / n - mean() * mean();
        return spread > 0.0 ? spread : 0.0;
    }

    [[nodiscard]] double standardDeviation() const noexcept { return std::sqrt(variance()); }

    /// Highest sample at or below the given percentile, clamped to [0, 100].
    ///
    /// Reports the top of the bucket a percentile falls into, so the answer
    /// never understates the sample it stands for. The error is bounded by the
    /// resolution the histogram was configured with.
    ///
    /// The result is then clamped to the range actually observed. A bucket's top
    /// edge can sit above the largest sample recorded -- the sample of 200 ticks
    /// lands in a bucket reaching 204 -- and a report claiming a 99th percentile
    /// larger than its own maximum is not a more accurate answer, it is a wrong
    /// one.
    [[nodiscard]] std::uint64_t valueAtPercentile(double percentile) const noexcept {
        if (count_ == 0) {
            return 0;
        }
        if (percentile <= 0.0) {
            return min_;
        }
        const auto target =
            static_cast<std::uint64_t>(std::min(percentile, 100.0) / 100.0 * static_cast<double>(count_));

        std::uint64_t bucketTop = max_;
        std::uint64_t observed = 0;
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            observed += slots_[index];
            if (observed > target) {
                bucketTop = highestEquivalentValue(index);
                break;
            }
        }

        return std::min(bucketTop, std::max(min_, max_));
    }

    [[nodiscard]] std::uint64_t median() const noexcept { return valueAtPercentile(50.0); }
    [[nodiscard]] std::uint64_t p90() const noexcept { return valueAtPercentile(90.0); }
    [[nodiscard]] std::uint64_t p99() const noexcept { return valueAtPercentile(99.0); }
    [[nodiscard]] std::uint64_t p999() const noexcept { return valueAtPercentile(99.9); }

    /// Heap the histogram occupies, in bytes.
    [[nodiscard]] std::size_t memoryUsage() const noexcept { return slots_.size() * sizeof(std::uint32_t); }

    /// Heap a histogram with the given geometry would occupy, without building it.
    [[nodiscard]] static std::size_t memoryUsageFor(std::uint64_t highestTrackableValue,
                                                    std::uint8_t significantFigures) noexcept {
        return computeLayout(highestTrackableValue, significantFigures).slotCount * sizeof(std::uint32_t);
    }

    /// Smallest and largest values that land in the same bucket as `value`.
    ///
    /// Every value the histogram reports for that bucket lies between the two, so
    /// this is the accuracy a percentile answer comes with.
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> bucketBoundsFor(std::uint64_t value) const noexcept {
        const auto index = slotFor(value);
        return {lowestEquivalentValue(layout_, index), highestEquivalentValue(layout_, index)};
    }

    /// Upper bound on the relative error of any reported percentile, as a
    /// fraction of the value itself.
    ///
    /// A sample can only be off by the width of the slot it landed in, and that
    /// width shrinks as the value climbs, so one bound covers the whole range:
    /// 6.25% at the default one significant figure, under 0.8% at two.
    [[nodiscard]] double relativeErrorBound() const noexcept {
        return 1.0 / static_cast<double>(layout_.subBucketHalfCount);
    }

private:
    static constexpr std::uint64_t kUnset = ~0ull;

    /// Slot geometry, derived once at construction and never written again.
    struct Layout {
        std::uint8_t subBucketHalfCountMagnitude;
        std::uint32_t subBucketCount;
        std::uint32_t subBucketHalfCount;
        std::uint32_t slotCount;
        std::uint32_t saturatedIndex;
        std::uint64_t highestTrackableValue;
    };

    static constexpr std::uint64_t powerOfTen(std::uint8_t exponent) noexcept {
        return (exponent == 0)   ? 1ull
               : (exponent == 1) ? 10ull
               : (exponent == 2) ? 100ull
               : (exponent == 3) ? 1'000ull
               : (exponent == 4) ? 10'000ull
                                  : 100'000ull;
    }

    /// Maps a value onto its slot, folding everything past the top of the range
    /// into the boundary slot so percentiles keep reporting a bounded number.
    [[nodiscard]] static constexpr std::uint32_t slotFor(const Layout& layout, std::uint64_t value) noexcept {
        if (value >= layout.highestTrackableValue) {
            return layout.saturatedIndex;
        }
        return rawSlotFor(layout, value);
    }

    /// Maps an in-range value onto its slot.
    [[nodiscard]] static constexpr std::uint32_t rawSlotFor(const Layout& layout, std::uint64_t value) noexcept {
        const std::uint32_t bucketIndex = bucketFor(layout, value);
        const auto subBucketIndex = static_cast<std::uint32_t>(value >> bucketIndex);
        const auto baseIndex = (bucketIndex + 1) << layout.subBucketHalfCountMagnitude;
        return baseIndex + (subBucketIndex - layout.subBucketHalfCount);
    }

    /// The bucket a value belongs to. Values below `subBucketCount` get one slot
    /// each; above that, the bucket index falls straight out of the value's width.
    [[nodiscard]] static constexpr std::uint32_t bucketFor(const Layout& layout, std::uint64_t value) noexcept {
        if (value < layout.subBucketCount) {
            return 0;
        }
        return static_cast<std::uint32_t>(std::bit_width(value)) - layout.subBucketHalfCountMagnitude - 1;
    }

    /// Smallest value that can land in a slot, the inverse of `slotFor`.
    [[nodiscard]] static constexpr std::uint64_t lowestEquivalentValue(const Layout& layout,
                                                                      std::size_t index) noexcept {
        if (index < layout.subBucketCount) {
            return index;
        }
        return static_cast<std::uint64_t>(subBucketOf(layout, index)) << bucketOf(layout, index);
    }

    /// Largest value that can land in a slot.
    [[nodiscard]] static constexpr std::uint64_t highestEquivalentValue(const Layout& layout,
                                                                       std::size_t index) noexcept {
        if (index < layout.subBucketCount) {
            return index;
        }
        return ((static_cast<std::uint64_t>(subBucketOf(layout, index)) + 1) << bucketOf(layout, index)) - 1;
    }

    /// The sub-index a slot covers, within `[subBucketHalfCount, subBucketCount)`.
    [[nodiscard]] static constexpr std::uint32_t subBucketOf(const Layout& layout,
                                                                std::size_t index) noexcept {
        return static_cast<std::uint32_t>(index & (layout.subBucketHalfCount - 1)) +
               layout.subBucketHalfCount;
    }

    /// The bucket a slot covers.
    [[nodiscard]] static constexpr std::uint32_t bucketOf(const Layout& layout,
                                                             std::size_t index) noexcept {
        return static_cast<std::uint32_t>(index >> layout.subBucketHalfCountMagnitude) - 1;
    }

    static constexpr Layout computeLayout(std::uint64_t highestTrackableValue,
                                           std::uint8_t significantFigures) noexcept {
        const auto digits = static_cast<std::uint8_t>(std::clamp<std::uint16_t>(significantFigures, 1, 5));

        // The axis gets 2*10^digits linear slots before the doubling begins; the
        // next power of two above that count sets the width of the first bucket.
        const std::uint64_t slotsForFullResolution = 2 * powerOfTen(digits);
        const auto subBucketCountMagnitude = static_cast<std::uint8_t>(std::bit_width(slotsForFullResolution - 1));
        const auto subBucketHalfCountMagnitude = static_cast<std::uint8_t>(subBucketCountMagnitude - 1);
        const auto subBucketCount = std::uint32_t{1} << subBucketCountMagnitude;
        const auto subBucketHalfCount = subBucketCount / 2;

        // Grow the bucket count until the smallest value no bucket can hold is
        // already above the range we promise to track.
        std::uint64_t smallestUntrackable = subBucketCount;
        std::uint32_t bucketCount = 1;
        while (smallestUntrackable <= highestTrackableValue) {
            smallestUntrackable <<= 1;
            ++bucketCount;
        }

        Layout layout{subBucketHalfCountMagnitude, subBucketCount, subBucketHalfCount,
                      (bucketCount + 1) * subBucketHalfCount, 0, highestTrackableValue};
        layout.saturatedIndex = rawSlotFor(layout, highestTrackableValue);
        return layout;
    }

    [[nodiscard]] std::size_t slotFor(std::uint64_t value) const noexcept { return slotFor(layout_, value); }

    [[nodiscard]] std::uint64_t highestEquivalentValue(std::size_t index) const noexcept {
        return highestEquivalentValue(layout_, index);
    }

    Layout layout_;
    std::vector<std::uint32_t> slots_;
    std::uint64_t count_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t min_ = kUnset;
    std::uint64_t max_ = 0;
};

} // namespace friction