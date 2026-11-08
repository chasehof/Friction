//
//  Friction 0.1.0 -- single-header distribution.
//
//  Generated from include/friction/ by tools/amalgamate.py. Do not edit.
//  See the repository for the sources and the documentation.
//

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ============ friction/detail/platform.hpp ============

///
/// \file platform.hpp
///
/// Compiler, architecture and operating system detection.
///
/// Every Friction detail header includes this first and may rely on the macros
/// defined here. Exactly one `FRICTION_COMPILER_*` and one `FRICTION_OS_*` macro
/// is defined; architecture macros are only defined for the architecture that
/// actually provides an accelerated clock.
///
///
#include <cstddef>

// ---------------------------------------------------------------- compiler --

#if defined(_MSC_VER)
#define FRICTION_COMPILER_MSVC 1
#elif defined(__clang__)
#define FRICTION_COMPILER_CLANG 1
#elif defined(__GNUC__)
#define FRICTION_COMPILER_GCC 1
#else
#define FRICTION_COMPILER_UNKNOWN 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#define FRICTION_LIKELY(x) __builtin_expect(!!(x), 1)
#define FRICTION_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define FRICTION_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define FRICTION_LIKELY(x) (!!(x))
#define FRICTION_UNLIKELY(x) (!!(x))
#define FRICTION_NOINLINE __declspec(noinline)
#else
#define FRICTION_LIKELY(x) (!!(x))
#define FRICTION_UNLIKELY(x) (!!(x))
#define FRICTION_NOINLINE
#endif

// ------------------------------------------------------------ architecture --

#if defined(__x86_64__) || defined(_M_X64)
#define FRICTION_ARCH_X86_64 1
#elif defined(__i386__) || defined(_M_IX86)
#define FRICTION_ARCH_X86_32 1
#endif

#if defined(FRICTION_ARCH_X86_64) || defined(FRICTION_ARCH_X86_32)
#if defined(FRICTION_COMPILER_MSVC)
#include <intrin.h>
#endif
#endif

// --------------------------------------------------------------------- os --

#if defined(__linux__)
#define FRICTION_OS_LINUX 1
#elif defined(__APPLE__)
#define FRICTION_OS_APPLE 1
#elif defined(_WIN32)
#define FRICTION_OS_WINDOWS 1
#endif

namespace friction::detail {

/// Bytes in a cache line. Counters are padded to this to keep unrelated
/// counters from sharing a line.
inline constexpr std::size_t kCacheLineSize = 64;

/// Hint to the CPU that we are in a spin-wait loop.
inline void cpuRelax() noexcept {
#if defined(FRICTION_ARCH_X86_64) || defined(FRICTION_ARCH_X86_32)
#if defined(FRICTION_COMPILER_MSVC)
    _mm_pause();
#else
    __builtin_ia32_pause();
#endif
#endif
}

} // namespace friction::detail


// ============ friction/clock.hpp ============

///
/// \file clock.hpp
///
/// The tick source Friction measures with.
///
/// On x86 this is the time stamp counter read with `lfence; rdtsc`, which costs
/// roughly 20-30 cycles and keeps sub-microsecond resolution. Elsewhere, and on
/// x86 parts whose TSC is not invariant, it degrades to a `std::chrono::
/// steady_clock` reading counted in nanoseconds. In both cases `frequency()`
/// reports the number of ticks in one second, so all conversions are uniform.
///


#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>

#if defined(FRICTION_ARCH_X86_64) || defined(FRICTION_ARCH_X86_32)
#if defined(FRICTION_COMPILER_MSVC)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace friction {

class Clock;

namespace detail {

/// Host-provided tick rate, or zero when the rate should be calibrated.
std::atomic<std::uint64_t>& ticksPerSecondOverride() noexcept;

/// True when the time stamp counter runs at a fixed rate across all cores and
/// across the whole life of the machine, which is the only condition under which
/// its readings stay comparable over time.
inline bool hasInvariantTimeStampCounter() noexcept {
#if !defined(FRICTION_ARCH_X86_64) && !defined(FRICTION_ARCH_X86_32)
    return false;
#elif defined(FRICTION_COMPILER_MSVC)
    // CPUID leaf 0x80000007, EDX bit 8: invariant time stamp counter. This is
    // the architectural answer, and the only trustworthy one -- deriving the rate
    // from a brand string would not survive a power management policy change.
    int registers[4]{};
    __cpuid(registers, 0x80000007);
    return (registers[3] & (1 << 8)) != 0;
#else
    static const bool invariant = [] {
        unsigned eax = 0;
        unsigned ebx = 0;
        unsigned ecx = 0;
        unsigned edx = 0;
        if (__get_cpuid(0x80000007u, &eax, &ebx, &ecx, &edx) == 0) {
            return false;
        }
        return (edx & (1u << 8)) != 0;
    }();
    return invariant;
#endif
}

/// Calibrates the tick rate against `std::chrono::steady_clock`.
std::uint64_t calibrateFrequency() noexcept;

} // namespace detail

/// Monotonic tick source used by every Friction measurement.
class Clock final {
public:
    /// Raw tick counter. Only differences between two readings are meaningful.
    using Ticks = std::uint64_t;

    /// Ticks per second for the portable `steady_clock` fallback.
    static constexpr Ticks kTicksPerSecondFallback = 1'000'000'000ull;

    /// Reads the tick counter.
    ///
    /// This is the only function on the measurement hot path. It performs no
    /// allocation, takes no locks and touches no shared mutable state.
    static Ticks now() noexcept {
#if defined(FRICTION_ARCH_X86_64) || defined(FRICTION_ARCH_X86_32)
        return readTimeStampCounter();
#else
        return steadyClockTicks();
#endif
    }

    /// Number of ticks in one second for the source returned by `now()`.
    ///
    /// The first call calibrates against `std::chrono::steady_clock` and costs a
    /// few milliseconds; every later call is a single atomic load. Hosts that
    /// know the TSC rate should call `setFrequency()` during startup to skip
    /// calibration entirely.
    static Ticks frequency() noexcept {
        static const Ticks calibrated = detail::calibrateFrequency();
        return calibrated;
    }

    /// Overrides the tick rate reported by `frequency()`.
    ///
    /// Must be called before anything reads `frequency()`, and never while a
    /// session is recording: rescaling mid-flight would corrupt the readings
    /// already taken. Passing zero restores the calibrated rate.
    static void setFrequency(Ticks ticksPerSecond) noexcept {
        detail::ticksPerSecondOverride().store(ticksPerSecond, std::memory_order_relaxed);
    }

    /// True when `now()` is backed by the hardware time stamp counter rather than
    /// by a library clock.
    static bool isHardwareCounter() noexcept { return detail::hasInvariantTimeStampCounter(); }

    /// Converts a tick delta to whole nanoseconds.
    static std::uint64_t toNanoseconds(Ticks ticks, Ticks frequency) noexcept {
        if (frequency == 0) {
            return 0;
        }
        const Ticks wholeSeconds = ticks / frequency;
        const Ticks remainder = ticks - wholeSeconds * frequency;
        return wholeSeconds * kTicksPerSecondFallback +
               static_cast<Ticks>(static_cast<double>(remainder) * 1.0e9 / static_cast<double>(frequency));
    }

    /// Converts a tick delta to fractional nanoseconds.
    static double toNanosecondsExact(Ticks ticks, Ticks frequency) noexcept {
        if (frequency == 0) {
            return 0.0;
        }
        return static_cast<double>(ticks) * static_cast<double>(kTicksPerSecondFallback) /
               static_cast<double>(frequency);
    }

    /// Converts a tick delta to milliseconds.
    static double toMilliseconds(Ticks ticks, Ticks frequency) noexcept {
        return toNanosecondsExact(ticks, frequency) / 1.0e6;
    }

private:
    Clock() = delete;

    static std::uint64_t steadyClockTicks() noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

#if defined(FRICTION_ARCH_X86_64) || defined(FRICTION_ARCH_X86_32)

    /// `lfence` keeps speculatively executed work from being counted as elapsed
    /// time before the read. It does not otherwise serialize the pipeline.
    static std::uint64_t readTimeStampCounter() noexcept {
#if defined(FRICTION_COMPILER_MSVC)
        _mm_lfence();
        return static_cast<std::uint64_t>(__rdtsc());
#elif defined(FRICTION_ARCH_X86_64)
        std::uint32_t low = 0;
        std::uint32_t high = 0;
        __asm__ __volatile__("lfence\n\trdtsc" : "=a"(low), "=d"(high) : : "memory");
        return (static_cast<std::uint64_t>(high) << 32) | low;
#else
        std::uint64_t ticks = 0;
        __asm__ __volatile__("lfence\n\trdtsc" : "=A"(ticks) : : "memory");
        return ticks;
#endif
    }

#endif
};

namespace detail {

inline std::atomic<std::uint64_t>& ticksPerSecondOverride() noexcept {
    static std::atomic<std::uint64_t> override{0};
    return override;
}

/// Calibrates the tick rate against `std::chrono::steady_clock`.
///
/// Takes the median of several short rounds because a single round is at the
/// mercy of the scheduler preempting the thread mid-measurement. That error is
/// one-sided: the wall clock advances while our ticks keep running, so a
/// preempted round always under-reports the frequency and would bias every
/// timing Friction reports low.
inline std::uint64_t calibrateFrequency() noexcept {
    if (const auto overridden = ticksPerSecondOverride().load(std::memory_order_relaxed); overridden != 0) {
        return overridden;
    }

#if !defined(FRICTION_ARCH_X86_64) && !defined(FRICTION_ARCH_X86_32)
    return Clock::kTicksPerSecondFallback;
#else
    if (!Clock::isHardwareCounter()) {
        return Clock::kTicksPerSecondFallback;
    }

    static constexpr int kRounds = 5;
    static constexpr auto kRoundDuration = std::chrono::milliseconds(2);

    std::array<double, kRounds> estimates{};
    std::size_t collected = 0;

    for (int round = 0; round < kRounds && collected < estimates.size(); ++round) {
        const auto wallStart = std::chrono::steady_clock::now();
        const Clock::Ticks tickStart = Clock::now();
        const auto deadline = wallStart + kRoundDuration;
        while (std::chrono::steady_clock::now() < deadline) {
            cpuRelax();
        }
        const Clock::Ticks tickEnd = Clock::now();
        const auto wallEnd = std::chrono::steady_clock::now();

        const double seconds = std::chrono::duration<double>(wallEnd - wallStart).count();
        if (seconds <= 0.0) {
            continue;
        }
        estimates[collected++] = static_cast<double>(tickEnd - tickStart) / seconds;
    }

    if (collected == 0) {
        return Clock::kTicksPerSecondFallback;
    }

    std::sort(estimates.begin(), estimates.begin() + static_cast<std::ptrdiff_t>(collected));
    return static_cast<Clock::Ticks>(estimates[collected / 2] + 0.5);
#endif
}

} // namespace detail
} // namespace friction


// ============ friction/counters.hpp ============

///
/// \file counters.hpp
///
/// Counters for things a timer cannot see: allocations, cache misses, retries,
/// bytes on the wire.
///
/// Every counter is split into cache-line-padded shards and the calling thread
/// only ever touches its own. That turns a single `lock xadd` on one shared
/// word -- which serializes as soon as two cores are incrementing -- into an
/// uncontended store to a private line. Reading sums the shards, which is the
/// only step that has to think about concurrency at all.
///
///

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace friction {

/// A counter safe to increment from any thread without contention.
class Counter final {
public:
    /// Number of shards. Must be a power of two.
    static constexpr std::size_t kShardCount = 8;

    static_assert((kShardCount & (kShardCount - 1)) == 0, "kShardCount must be a power of two");

    /// Increments by `delta`, or by one if none is given.
    void add(std::uint64_t delta = 1) noexcept {
        shard().value.fetch_add(delta, std::memory_order_relaxed);
    }

    void operator+=(std::uint64_t delta) noexcept { add(delta); }

    /// Sum across every shard. Costs one pass over the shards, so it belongs off
    /// the hot path.
    [[nodiscard]] std::uint64_t value() const noexcept {
        std::uint64_t total = 0;
        for (const auto& shard : shards_) {
            total += shard.value.load(std::memory_order_relaxed);
        }
        return total;
    }

    /// This thread's contribution alone.
    [[nodiscard]] std::uint64_t valueOnCallingThread() const noexcept {
        return shard().value.load(std::memory_order_relaxed);
    }

    /// Zeroes every shard. Only meaningful once the owning threads have quiesced.
    void reset() noexcept {
        for (auto& shard : shards_) {
            shard.value.store(0, std::memory_order_relaxed);
        }
    }

private:
    /// One counter line. Padding keeps two counters from landing on the same one.
    struct alignas(detail::kCacheLineSize) Shard {
        std::atomic<std::uint64_t> value{0};
    };

    [[nodiscard]] static std::size_t shardIndex() noexcept {
        // Threads are numbered in the order they first touch Friction and the
        // numbering is stable for the life of the thread, so a thread keeps
        // hitting the same shard. Threads past the shard count wrap around and
        // share; correctness is unaffected, only the contention profile is.
        static std::atomic<std::size_t> nextThreadOrdinal{0};
        static thread_local std::size_t ordinal = nextThreadOrdinal.fetch_add(1, std::memory_order_relaxed);
        return ordinal & (kShardCount - 1);
    }

    [[nodiscard]] const Shard& shard() const noexcept { return shards_[shardIndex()]; }

    [[nodiscard]] Shard& shard() noexcept { return shards_[shardIndex()]; }

    Shard shards_[kShardCount];
};

/// A named collection of counters.
///
/// Names are resolved when a counter is first asked for and the returned
/// reference stays valid for the life of the set, so callers hoist the reference
/// out and only ever pay the lookup once during setup.
///
/// Registering a new name appends to the set and is therefore not thread-safe;
/// do it before starting the threads that will increment.
class CounterSet final {
public:
    /// Returns the counter called `name`, creating it if this set has not seen it.
    [[nodiscard]] Counter& counter(std::string_view name) {
        for (std::size_t index = 0; index < names_.size(); ++index) {
            if (names_[index] == name) {
                return *counters_[index];
            }
        }
        names_.emplace_back(name);
        counters_.push_back(std::make_unique<Counter>());
        return *counters_.back();
    }

    /// Name of the counter at `index`, in registration order.
    [[nodiscard]] std::string_view name(std::size_t index) const noexcept { return names_[index]; }

    [[nodiscard]] std::size_t size() const noexcept { return counters_.size(); }

    [[nodiscard]] bool contains(std::string_view name) const noexcept {
        for (const auto& candidate : names_) {
            if (candidate == name) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::uint64_t value(std::size_t index) const noexcept { return counters_[index]->value(); }

    [[nodiscard]] Counter& operator[](std::size_t index) noexcept { return *counters_[index]; }

    /// Zeroes every counter.
    void reset() noexcept {
        for (auto& counter : counters_) {
            counter->reset();
        }
    }

private:
    // unique_ptr keeps each Counter's address fixed as the vector reallocates.
    std::vector<std::string> names_;
    std::vector<std::unique_ptr<Counter>> counters_;
};

} // namespace friction


// ============ friction/histogram.hpp ============

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


// ============ friction/process.hpp ============

///
/// \file process.hpp
///
/// Numbers the operating system knows about this process.
///
/// A timer can only see the region you wrapped. These are the things a timing
/// scope cannot: how much memory the process is holding, how often it is being
/// scheduled out, how much work the kernel is doing on its behalf, and how much
/// CPU it is actually burning. Together they are what tells you whether a slow
/// function is slow because of its own instructions or because of the allocator,
/// the page faults, or the contention around it.
///
/// Everything here is a read-only sample of kernel counters. Reading them costs
/// the profiled process a couple of syscalls, so they are sampled when a session
/// starts and stops rather than continuously.
///
///

#include <cstddef>
#include <cstdint>
#include <string_view>

#if defined(FRICTION_OS_LINUX)
#include <cstring>

#include <fcntl.h>
#include <unistd.h>
#endif

#if defined(FRICTION_OS_LINUX) || defined(FRICTION_OS_APPLE)
#include <sys/resource.h>
#include <sys/time.h>
#endif

namespace friction {

/// Where a set of process numbers came from.
enum class ProcessSource {
    unknown, ///< Nothing is available on this platform.
    procfs,  ///< Linux, reading /proc/self.
    rusage,  ///< CPU time and faults from getrusage, nothing else.
};

/// A sample of this process's resource counters.
struct ProcessStats {
    ProcessSource source = ProcessSource::unknown;

    /// Resident set size, and the high water mark it reached since start.
    std::uint64_t residentBytes = 0;
    std::uint64_t peakResidentBytes = 0;

    /// Address space reserved, which includes mappings not backed by memory.
    std::uint64_t virtualBytes = 0;

    /// Memory the kernel had to fetch from disk.
    std::uint64_t majorFaults = 0;

    /// Memory the kernel satisfied from around the process.
    std::uint64_t minorFaults = 0;

    /// Times the process gave up the CPU of its own accord, usually on a read.
    std::uint64_t voluntaryContextSwitches = 0;

    /// Times the process was preempted. A climbing count under a flat CPU
    /// profile means the thread is starved rather than slow.
    std::uint64_t involuntaryContextSwitches = 0;

    /// CPU time consumed in user mode and in the kernel.
    std::uint64_t userCpuNanoseconds = 0;
    std::uint64_t systemCpuNanoseconds = 0;

    /// Threads the kernel currently has for this process.
    std::uint32_t threadCount = 0;

    /// True when resident memory was reported.
    [[nodiscard]] bool hasResidentMemory() const noexcept {
        return source == ProcessSource::procfs && residentBytes != 0;
    }

    /// True when scheduling counters were reported.
    [[nodiscard]] bool hasContextSwitches() const noexcept {
        return source == ProcessSource::procfs && (voluntaryContextSwitches != 0 || involuntaryContextSwitches != 0);
    }

    /// CPU time from both modes.
    [[nodiscard]] std::uint64_t totalCpuNanoseconds() const noexcept {
        return userCpuNanoseconds + systemCpuNanoseconds;
    }
};

/// Reads the current process's resource counters.
///
/// Never throws and never allocates: on failure it returns an all-zero sample
/// whose `source` says so, rather than pretending the process is idle.
[[nodiscard]] ProcessStats readProcessStats() noexcept;

/// True when this build can read real process counters from the platform.
[[nodiscard]] bool processStatsSupported() noexcept;

namespace detail {

#if defined(FRICTION_OS_LINUX)

/// A `/proc` file read into a fixed buffer, so sampling cannot allocate.
class ProcFile final {
public:
    ProcFile() noexcept = default;

    ~ProcFile() noexcept;

    ProcFile(const ProcFile&) = delete;
    ProcFile& operator=(const ProcFile&) = delete;

    /// Reads `path`, truncating rather than growing if it is unusually long.
    [[nodiscard]] bool open(const char* path) noexcept;

    /// Finds a line starting with `key` and parses the number on it.
    [[nodiscard]] bool findUnsigned(std::string_view key, std::uint64_t& out) const noexcept;

    /// Finds a line starting with `key` and parses its value with its unit,
    /// scaling the kB, MB and GB suffixes `/proc` uses to bytes.
    [[nodiscard]] bool findByteCount(std::string_view key, std::uint64_t& out) const noexcept;

    /// Parses a field of `/proc/self/stat` by its number in proc(5), where
    /// fields 1 and 2 are the pid and the parenthesised command name.
    [[nodiscard]] bool readStatField(std::size_t field, std::uint64_t& out) const noexcept;

private:
    static constexpr std::size_t kCapacity = 8192;

    [[nodiscard]] const char* end() const noexcept { return buffer_ + size_; }

    /// Locates the start of the value following `key`, or null if the key is
    /// absent. Only matches at the start of a line, so `RssAnon` cannot be
    /// mistaken for `VmRSS`.
    [[nodiscard]] const char* findValueFor(std::string_view key) const noexcept;

    [[nodiscard]] static const char* skipSpaces(const char* cursor, const char* end) noexcept;
    /// Parses a decimal integer at `cursor` and leaves it on the first character
    /// that is not part of the number, so the caller can read a unit suffix.
    [[nodiscard]] static bool parseUnsigned(const char*& cursor, const char* end,
                                           std::uint64_t& out) noexcept;

    char buffer_[kCapacity]{};
    std::size_t size_ = 0;
    int descriptor_ = -1;
};

#endif // FRICTION_OS_LINUX

} // namespace detail

#if defined(FRICTION_OS_LINUX)

namespace detail {

inline ProcFile::~ProcFile() noexcept {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
    }
}

inline bool ProcFile::open(const char* path) noexcept {
    descriptor_ = ::open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor_ < 0) {
        return false;
    }
    size_ = 0;
    while (size_ < kCapacity) {
        const auto read = ::read(descriptor_, buffer_ + size_, kCapacity - size_);
        if (read <= 0) {
            break;
        }
        size_ += static_cast<std::size_t>(read);
    }
    return size_ > 0;
}

inline const char* ProcFile::skipSpaces(const char* cursor, const char* end) noexcept {
    while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
        ++cursor;
    }
    return cursor;
}

inline bool ProcFile::parseUnsigned(const char*& cursor, const char* end, std::uint64_t& out) noexcept {
    cursor = skipSpaces(cursor, end);
    if (cursor == end || *cursor < '0' || *cursor > '9') {
        return false;
    }
    std::uint64_t value = 0;
    while (cursor < end && *cursor >= '0' && *cursor <= '9') {
        value = value * 10 + static_cast<std::uint64_t>(*cursor - '0');
        ++cursor;
    }
    out = value;
    return true;
}

inline const char* ProcFile::findValueFor(std::string_view key) const noexcept {
    std::size_t offset = 0;
    while (offset < size_) {
        if (std::memcmp(buffer_ + offset, key.data(), key.size()) == 0 &&
            (offset == 0 || buffer_[offset - 1] == '\n')) {
            return buffer_ + offset + key.size();
        }
        const auto* lineBreak = static_cast<const char*>(
            std::memchr(buffer_ + offset, '\n', size_ - offset));
        if (lineBreak == nullptr) {
            return nullptr;
        }
        offset = static_cast<std::size_t>(lineBreak - buffer_) + 1;
    }
    return nullptr;
}

inline bool ProcFile::findUnsigned(std::string_view key, std::uint64_t& out) const noexcept {
    const char* cursor = findValueFor(key);
    return cursor != nullptr && parseUnsigned(cursor, end(), out);
}

inline bool ProcFile::findByteCount(std::string_view key, std::uint64_t& out) const noexcept {
    const char* cursor = findValueFor(key);
    if (cursor == nullptr || !parseUnsigned(cursor, end(), out)) {
        return false;
    }

    std::uint64_t scale = 1;
    const char* const unit = skipSpaces(cursor, end());
    if (unit != end()) {
        switch (*unit) {
        case 'k':
        case 'K':
            scale = 1ull << 10;
            break;
        case 'm':
        case 'M':
            scale = 1ull << 20;
            break;
        case 'g':
        case 'G':
            scale = 1ull << 30;
            break;
        default:
            break;
        }
    }
    out *= scale;
    return true;
}

inline bool ProcFile::readStatField(std::size_t field, std::uint64_t& out) const noexcept {
    // Field 2 is the command name: parenthesised, and free to contain spaces, so
    // everything after it has to be located past the closing parenthesis.
    if (field < 3) {
        return false;
    }
    const char* closing = nullptr;
    for (std::size_t index = size_; index > 0; --index) {
        if (buffer_[index - 1] == ')') {
            closing = buffer_ + index - 1;
            break;
        }
    }
    if (closing == nullptr) {
        return false;
    }

    const char* cursor = skipSpaces(closing + 1, end());
    for (std::size_t index = 3; index < field; ++index) {
        while (cursor < end() && *cursor != ' ') {
            ++cursor;
        }
        cursor = skipSpaces(cursor, end());
    }
    return parseUnsigned(cursor, end(), out);
}

} // namespace detail

#endif // FRICTION_OS_LINUX

#if defined(FRICTION_OS_LINUX) || defined(FRICTION_OS_APPLE)

namespace detail {

/// `getrusage` splits CPU time into whole seconds and microseconds, so the two
/// have to be recombined and scaled to nanoseconds.
[[nodiscard]] inline std::uint64_t timevalToNanoseconds(const timeval& value) noexcept {
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(value.tv_usec) * 1'000ull;
}

/// Fills in the CPU time and page fault counters every POSIX platform provides.
inline void readResourceUsage(ProcessStats& stats) noexcept {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return;
    }
    stats.userCpuNanoseconds = timevalToNanoseconds(usage.ru_utime);
    stats.systemCpuNanoseconds = timevalToNanoseconds(usage.ru_stime);
    stats.minorFaults = static_cast<std::uint64_t>(usage.ru_minflt);
    stats.majorFaults = static_cast<std::uint64_t>(usage.ru_majflt);
}

} // namespace detail

#endif // FRICTION_OS_LINUX || FRICTION_OS_APPLE

[[nodiscard]] inline bool processStatsSupported() noexcept {
#if defined(FRICTION_OS_LINUX) || defined(FRICTION_OS_APPLE)
    return true;
#else
    return false;
#endif
}

[[nodiscard]] inline ProcessStats readProcessStats() noexcept {
    ProcessStats stats;

#if defined(FRICTION_OS_LINUX)
    stats.source = ProcessSource::procfs;

    // /proc/self/status carries memory and scheduling in one read; the field we
    // want for threads lives in /proc/self/stat, where it costs a second read.
    detail::ProcFile status;
    if (status.open("/proc/self/status")) {
        const bool sawResident = status.findByteCount("VmRSS:", stats.residentBytes);
        (void)status.findByteCount("VmHWM:", stats.peakResidentBytes);
        (void)status.findByteCount("VmSize:", stats.virtualBytes);
        (void)status.findUnsigned("voluntary_ctxt_switches:", stats.voluntaryContextSwitches);
        (void)status.findUnsigned("nonvoluntary_ctxt_switches:", stats.involuntaryContextSwitches);
        if (!sawResident) {
            stats.residentBytes = 0;
        }
    }

    detail::ProcFile stat;
    if (stat.open("/proc/self/stat")) {
        std::uint64_t threads = 0;
        if (stat.readStatField(20, threads)) {
            stats.threadCount = static_cast<std::uint32_t>(threads);
        }
    }

    detail::readResourceUsage(stats);

#elif defined(FRICTION_OS_APPLE)
    stats.source = ProcessSource::rusage;
    detail::readResourceUsage(stats);

#else
    stats.source = ProcessSource::unknown;
#endif

    return stats;
}

} // namespace friction


// ============ friction/report.hpp ============

///
/// \file report.hpp
///
/// What a finished session produced.
///
/// Reports hold plain values: no references back into thread state, no
/// histograms, nothing that another session could disturb. A report can be
/// stored, copied, and serialized long after the process that produced it has
/// moved on.
///
/// All durations are in milliseconds and all samples in nanoseconds. Friction
/// measures in whatever unit the clock counts in and converts once, here, so
/// the hot path never touches a floating point value.
///
///

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace friction {

/// Everything measured for one named region, across every thread.
struct ZoneReport {
    /// Name exactly as written at the instrumentation site.
    std::string name;

    /// Threads that entered this region.
    std::size_t threadCount = 0;

    /// Entries into the region, from all threads.
    std::uint64_t callCount = 0;

    /// Time spent in the region with nested regions included.
    double inclusiveMilliseconds = 0.0;

    /// Time spent in the region itself, with nested regions subtracted.
    double selfMilliseconds = 0.0;

    double meanNanoseconds = 0.0;
    double standardDeviationNanoseconds = 0.0;

    std::uint64_t minNanoseconds = 0;
    std::uint64_t p50Nanoseconds = 0;
    std::uint64_t p90Nanoseconds = 0;
    std::uint64_t p99Nanoseconds = 0;
    std::uint64_t p999Nanoseconds = 0;
    std::uint64_t maxNanoseconds = 0;

    /// Share of the session's total self time spent in this region, 0 to 100.
    double selfPercent = 0.0;

    /// Mean time in the region for the threads that entered it.
    double meanCallsPerThread = 0.0;

    /// True when samples reached the top of the configured range, so the
    /// reported extremes are floors rather than measurements.
    bool saturated = false;
};

/// One counter's final value.
struct CounterReport {
    std::string name;
    std::uint64_t value = 0;
};

/// How this process's resource counters moved over the session.
struct ProcessReport {
    ProcessStats start;
    ProcessStats end;

    /// Wall time the session covered.
    double elapsedMilliseconds = 0.0;

    /// Resident memory and peak at the end of the session.
    std::uint64_t residentBytes = 0;
    std::uint64_t peakResidentBytes = 0;

    /// Change in resident memory across the session. A large positive number on
    /// a short session is a leak; a large negative one is a spike being released.
    std::int64_t residentGrowthBytes = 0;

    std::uint64_t minorFaults = 0;
    std::uint64_t majorFaults = 0;
    std::uint64_t voluntaryContextSwitches = 0;
    std::uint64_t involuntaryContextSwitches = 0;

    std::uint64_t userCpuMilliseconds = 0;
    std::uint64_t systemCpuMilliseconds = 0;

    /// CPU time as a share of wall time across all threads. Above 100% means
    /// more than one core was busy.
    double cpuUtilizationPercent = 0.0;

    std::uint32_t threadCount = 0;

    [[nodiscard]] bool supported() const noexcept { return start.source != ProcessSource::unknown; }
};

/// The result of one session.
class Report final {
public:
    /// Regions ordered by self time, descending. The first entry is where the
    /// time actually went.
    std::vector<ZoneReport> zones;

    /// Counters in the order they were first asked for.
    std::vector<CounterReport> counters;

    ProcessReport process;

    /// Threads that contributed at least one measurement.
    std::size_t threadCount = 0;

    /// Ticks per second the timings were scaled with, recorded so a report can
    /// be checked against the machine that produced it.
    Clock::Ticks clockFrequency = 0;

    /// True when `now()` was backed by the hardware counter.
    bool hardwareClock = false;

    [[nodiscard]] bool empty() const noexcept { return zones.empty() && counters.empty(); }

    /// Total self time across every region, in milliseconds.
    [[nodiscard]] double totalSelfMilliseconds() const noexcept {
        double total = 0.0;
        for (const auto& zone : zones) {
            total += zone.selfMilliseconds;
        }
        return total;
    }

    /// The region called `name`, or null if it was never measured.
    [[nodiscard]] const ZoneReport* find(std::string_view name) const noexcept {
        for (const auto& zone : zones) {
            if (zone.name == name) {
                return &zone;
            }
        }
        return nullptr;
    }

    /// The counter called `name`, or null if it was never registered.
    [[nodiscard]] const CounterReport* findCounter(std::string_view name) const noexcept {
        for (const auto& counter : counters) {
            if (counter.name == name) {
                return &counter;
            }
        }
        return nullptr;
    }

    /// Renders the report as a fixed-column table, slowest region first.
    [[nodiscard]] std::string toString() const;

    /// Renders the report as JSON, for writing to a file or shipping elsewhere.
    [[nodiscard]] std::string toJson(int indentWidth = 2) const;
};

} // namespace friction


// ============ friction/detail/thread_state.hpp ============

///
/// \file thread_state.hpp
///
/// The landing zone for everything one thread measures.
///
/// Samples are recorded into state owned by the thread that took them, so the
/// common case touches no atomics and takes no locks. Merging happens once, at
/// the end, off the hot path.
///
/// Zones are found by the address of the name literal at the instrumentation
/// site rather than by comparing name text. A call site passes the same address
/// every time it runs, so the lookup is a hash and a probe instead of a string
/// comparison, and a hot loop alternating between several instrumented regions
/// still costs the same as one that repeats a single region.
///
///

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace friction::detail {

/// One named region's accumulated samples for one thread.
struct ZoneSlot {
    ZoneSlot(std::string zoneName, const Histogram::Config& config)
        : name(std::move(zoneName)), inclusive(config), selfTime(config) {}

    /// Name exactly as written at the instrumentation site.
    std::string name;

    /// Time in the region, children included.
    Histogram inclusive;

    /// Time in the region with children subtracted.
    ///
    /// A region no child ever entered has identical inclusive and self time, but
    /// the second histogram is one allocation the library makes per region per
    /// thread up front rather than a branch and a potential allocation on the
    /// way out of every nested scope.
    Histogram selfTime;

    /// Adopts a new geometry and drops the samples recorded against the old one.
    void configure(const Histogram::Config& config) noexcept {
        inclusive.configure(config);
        selfTime.configure(config);
    }

    [[nodiscard]] std::string_view nameView() const noexcept { return name; }
};

/// Everything one thread contributes to a session.
class ThreadState final {
public:
    /// Instrumented regions nested deeper than this are measured but stop
    /// charging their time to a parent, so their time is not lost.
    static constexpr std::size_t kMaxNestingDepth = 64;

    ThreadState(Histogram::Config config, std::size_t ordinal) noexcept
        : config_(config), ordinal_(ordinal) {}

    ThreadState(const ThreadState&) = delete;
    ThreadState& operator=(const ThreadState&) = delete;

    /// Returns the slot for `name`, creating it the first time this thread
    /// measures that region. Valid until `reset()`.
    ZoneSlot& zone(std::string_view name) {
        std::size_t probe = probeSlotFor(name.data());

        for (std::size_t step = 0; step < kZoneTableSize; ++step) {
            const std::uint32_t entry = lookupTable_[probe];
            if (entry == kEmptySlot) {
                break;
            }
            ZoneSlot& candidate = *zones_[entry - 1];
            if (candidate.nameView().data() == name.data()) {
                return candidate;
            }
            probe = (probe + 1) & (kZoneTableSize - 1);
        }

        return resolveMiss(name);
    }

    void enter(ZoneSlot* slot) noexcept {
        if (depth_ < kMaxNestingDepth) {
            stack_[depth_] = Frame{slot, 0};
        }
        ++depth_;
    }

    /// Books the elapsed time of the innermost region.
    ///
    /// The region's inclusive time is what it measured. Its self time is what is
    /// left after subtracting the time its children spent inside it, which is
    /// only knowable here, as each child closes, by accumulating into the frame
    /// that opened before it.
    void leave(Clock::Ticks elapsed) noexcept {
        if (depth_ == 0) {
            return;
        }
        const std::size_t index = std::min(depth_, kMaxNestingDepth) - 1;
        Frame& frame = stack_[index];

        frame.slot->inclusive.record(elapsed);
        frame.slot->selfTime.record(elapsed > frame.childTicks ? elapsed - frame.childTicks : 0);

        if (index > 0) {
            // Hand the time to the enclosing region, so its self time comes out
            // already reduced by everything it contained.
            stack_[index - 1].childTicks += elapsed;
        }
        --depth_;
    }

    /// Drops every recorded sample and adopts the session's geometry, as when a
    /// new session begins.
    ///
    /// The slots themselves are kept rather than freed, so a scope that was open
    /// across the boundary still has something valid to write to. A session is
    /// expected to start with the previous work quiesced, but a thread that is
    /// unwinding through an instrumented frame should not be left holding a
    /// pointer into freed memory.
    void reset(const Histogram::Config& config) noexcept {
        config_ = config;
        for (auto& slot : zones_) {
            slot->configure(config);
        }
        depth_ = 0;
        lookupTable_.fill(kEmptySlot);
    }

    [[nodiscard]] const std::vector<std::unique_ptr<ZoneSlot>>& zones() const noexcept { return zones_; }
    [[nodiscard]] const Histogram::Config& histogramConfig() const noexcept { return config_; }
    [[nodiscard]] std::size_t ordinal() const noexcept { return ordinal_; }
    [[nodiscard]] std::size_t zoneCount() const noexcept { return zones_.size(); }

private:
    static constexpr std::size_t kZoneTableSize = 128; // power of two
    static constexpr std::uint32_t kEmptySlot = 0;

    /// One open scope, and the running total of what its children have spent.
    struct Frame {
        ZoneSlot* slot = nullptr;
        Clock::Ticks childTicks = 0;
    };

    /// Scatters string-literal addresses across the table. Finalizer mix, so a
    /// handful of call sites does not pile into one probe run.
    [[nodiscard]] static std::size_t probeSlotFor(const char* name) noexcept {
        auto key = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(name));
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdull;
        key ^= key >> 33;
        return static_cast<std::size_t>(key) & (kZoneTableSize - 1);
    }

    /// The innermost region we actually kept on the stack, or null if the
    /// nesting ran past the cap.
    [[nodiscard]] ZoneSlot* innermostStored() const noexcept {
        if (depth_ == 0 || depth_ > kMaxNestingDepth) {
            return nullptr;
        }
        return stack_[depth_ - 1].slot;
    }

    /// Handles a name the table has not seen. Two call sites can share a probe
    /// run, and more regions can be instrumented than the table has room for, so
    /// the slow path falls back to comparing the text before giving up.
    ZoneSlot& resolveMiss(std::string_view name) {
        for (auto& slot : zones_) {
            if (slot->nameView() == name) {
                return *slot;
            }
        }

        zones_.push_back(std::make_unique<ZoneSlot>(std::string(name), config_));
        ZoneSlot& created = *zones_.back();
        if (zones_.size() <= kZoneTableSize) {
            lookupTable_[probeSlotFor(name.data())] = static_cast<std::uint32_t>(zones_.size());
        }
        return created;
    }

    Histogram::Config config_;
    std::size_t ordinal_ = 0;
    std::size_t depth_ = 0;
    std::vector<std::unique_ptr<ZoneSlot>> zones_;
    std::array<Frame, kMaxNestingDepth> stack_{};
    std::array<std::uint32_t, kZoneTableSize> lookupTable_{};
};

} // namespace friction::detail


// ============ friction/detail/registry.hpp ============

///
/// \file registry.hpp
///
/// The set of threads that have ever measured.
///
/// A thread links itself in once, on the first time it records anything, with a
/// compare-and-swap that never blocks. After that first touch the thread does
/// no atomic traffic at all while it measures.
///
/// Nodes are intentionally never freed. They hold the only copy of the data a
/// worker thread collected before it exited, so holding on to them is what lets
/// a report still show a thread pool's worth of work after every worker has
/// already joined. The cost is bounded by the number of threads that ever
/// measured -- hundreds of bytes each -- and it lasts exactly as long as the
/// process.
///
///

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace friction::detail {

/// A node in the registry, reachable from the thread that created it.
///
/// The link is read while a session aggregates, so it is only ever published
/// once, by the owning thread, before that thread records anything.
struct RegisteredThread {
    RegisteredThread(std::size_t ordinal, const Histogram::Config& config) noexcept
        : state(config, ordinal) {}

    RegisteredThread* next = nullptr;
    ThreadState state;
};

/// Process-wide set of threads that have recorded a measurement.
class Registry final {
public:
    /// This thread's state, linking the thread in on first use.
    ///
    /// The pointer is held without a destructor so that exiting the thread does
    /// not take the node -- and the data in it -- back out of the registry.
    [[nodiscard]] static ThreadState& local(const Histogram::Config& config) noexcept {
        static thread_local RegisteredThread* node = nullptr;
        if (node == nullptr) {
            node = create(config);
        }
        return node->state;
    }

    /// Every registered thread, ordered by the sequence in which they first
    /// measured, so a report is reproducible run to run.
    [[nodiscard]] static std::vector<RegisteredThread*> snapshot() noexcept {
        std::vector<RegisteredThread*> nodes;

        for (auto* node = head().load(std::memory_order_acquire); node != nullptr;
             node = node->next) {
            nodes.push_back(node);
        }

        std::sort(nodes.begin(), nodes.end(),
                  [](const RegisteredThread* left, const RegisteredThread* right) {
                      return left->state.ordinal() < right->state.ordinal();
                  });
        return nodes;
    }

    [[nodiscard]] static std::size_t size() noexcept {
        std::size_t count = 0;
        for (auto* node = head().load(std::memory_order_acquire); node != nullptr; node = node->next) {
            ++count;
        }
        return count;
    }

    /// Frees every node and empties the registry.
    ///
    /// The registry deliberately outlives threads, so this exists for tests and
    /// for hosts that rebuild their world from scratch. Calling it while any
    /// thread is still measuring leaves dangling references.
    static void destroyAll() noexcept {
        auto* node = head().exchange(nullptr, std::memory_order_acq_rel);
        while (node != nullptr) {
            auto* next = node->next;
            delete node;
            node = next;
        }
    }

private:
    [[nodiscard]] static std::atomic<RegisteredThread*>& head() noexcept {
        static std::atomic<RegisteredThread*> registryHead{nullptr};
        return registryHead;
    }

    [[nodiscard]] static std::atomic<std::size_t>& nextOrdinal() noexcept {
        static std::atomic<std::size_t> ordinal{0};
        return ordinal;
    }

    [[nodiscard]] static RegisteredThread* create(const Histogram::Config& config) noexcept {
        const auto ordinal = nextOrdinal().fetch_add(1, std::memory_order_relaxed);
        auto* published = new RegisteredThread(ordinal, config);

        // Push onto the list. Another thread may have pushed between our load and
        // our store, in which case its list is the newer one and we retry.
        auto* observed = head().load(std::memory_order_relaxed);
        do {
            published->next = observed;
        } while (!head().compare_exchange_weak(observed, published, std::memory_order_release,
                                              std::memory_order_relaxed));

        return published;
    }

    Registry() = delete;
};

} // namespace friction::detail


// ============ friction/detail/report_format.hpp ============

///
/// \file report_format.hpp
///
/// Presentation for `friction::Report`.
///
/// Kept apart from `report.hpp` so the data model stays readable on its own. The
/// writers build strings rather than streaming, because a report is small and
/// the caller almost always wants the whole thing at once.
///
///

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace friction::detail {

/// Formats a millisecond count as seconds, which is the unit a session is
/// usually reasoned about in.
[[nodiscard]] inline std::string formatMilliseconds(double milliseconds) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << (milliseconds / 1'000.0) << 's';
    return out.str();
}

/// Formats a nanosecond count with the unit that suits its magnitude.
[[nodiscard]] inline std::string formatNanoseconds(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    if (value < 1'000.0) {
        out << value << "ns";
    } else if (value < 1'000'000.0) {
        out << (value / 1'000.0) << "us";
    } else if (value < 1'000'000'000.0) {
        out << (value / 1'000'000.0) << "ms";
    } else {
        out << (value / 1'000'000'000.0) << "s";
    }
    return out.str();
}

/// Overload for the samples, which are counts rather than averages.
[[nodiscard]] inline std::string formatNanoseconds(std::uint64_t nanoseconds) {
    return formatNanoseconds(static_cast<double>(nanoseconds));
}

/// Formats a byte count with a binary unit.
[[nodiscard]] inline std::string formatBytes(std::uint64_t bytes) {
    static constexpr std::array<const char*, 5> kUnits{"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < kUnits.size()) {
        value /= 1024.0;
        ++unit;
    }

    std::ostringstream out;
    out << std::fixed << std::setprecision(value < 10.0 && unit > 0 ? 2 : 1) << value << ' ' << kUnits[unit];
    return out.str();
}

/// Formats a signed byte delta, always carrying a sign so growth reads as a
/// change rather than as a total.
[[nodiscard]] inline std::string formatByteDelta(std::int64_t bytes) {
    const auto magnitude = static_cast<std::uint64_t>(bytes < 0 ? -bytes : bytes);
    return (bytes < 0 ? "-" : "+") + formatBytes(magnitude);
}

/// Minimal JSON writer: enough structure for a report, and nothing that lets a
/// non-finite double reach the output as invalid JSON.
class JsonWriter final {
public:
    explicit JsonWriter(int indentWidth) : indentWidth_(std::max(0, indentWidth)) {}

    void beginObject() {
        separate();
        buffer_ += '{';
        push();
    }

    void endObject() {
        pop();
        newline();
        buffer_ += '}';
    }

    void beginArray() {
        separate();
        buffer_ += '[';
        push();
    }

    void endArray() {
        pop();
        newline();
        buffer_ += ']';
    }

    void key(std::string_view name) {
        separate();
        buffer_ += '"';
        escape(name);
        buffer_ += "\": ";
        pendingValue_ = true;
    }

    void value(std::string_view text) {
        separate();
        buffer_ += '"';
        escape(text);
        buffer_ += '"';
    }

    void value(std::uint64_t number) {
        separate();
        buffer_ += std::to_string(number);
    }

    void value(std::int64_t number) {
        separate();
        buffer_ += std::to_string(number);
    }

    void value(double number) {
        separate();
        if (!std::isfinite(number)) {
            buffer_ += "null";
            return;
        }
        std::ostringstream out;
        out << std::setprecision(6) << number;
        buffer_ += out.str();
    }

    void value(bool flag) {
        separate();
        buffer_ += flag ? "true" : "false";
    }

    void member(std::string_view name, std::string_view text) {
        key(name);
        value(text);
    }

    void member(std::string_view name, const char* text) {
        key(name);
        value(std::string_view{text});
    }

    void member(std::string_view name, std::uint64_t number) {
        key(name);
        value(number);
    }

    void member(std::string_view name, std::int64_t number) {
        key(name);
        value(number);
    }

    void member(std::string_view name, double number) {
        key(name);
        value(number);
    }

    void member(std::string_view name, bool flag) {
        key(name);
        value(flag);
    }

    [[nodiscard]] std::string take() {
        newline();
        return std::move(buffer_);
    }

private:
    void push() {
        ++depth_;
        first_.push_back(true);
    }

    void pop() {
        if (depth_ > 0) {
            --depth_;
        }
        first_.pop_back();
    }

    /// Emits the comma and line break owed before the next value.
    void separate() {
        if (pendingValue_) {
            pendingValue_ = false;
            return;
        }
        if (first_.empty()) {
            return;
        }
        if (!first_.back()) {
            buffer_ += ',';
        }
        first_.back() = false;
        newline();
    }

    void newline() {
        if (indentWidth_ == 0) {
            return;
        }
        buffer_ += '\n';
        buffer_.append(static_cast<std::size_t>(depth_) * static_cast<std::size_t>(indentWidth_), ' ');
    }

    void escape(std::string_view text) {
        for (const char character : text) {
            switch (character) {
            case '"':
                buffer_ += "\\\"";
                break;
            case '\\':
                buffer_ += "\\\\";
                break;
            case '\n':
                buffer_ += "\\n";
                break;
            case '\r':
                buffer_ += "\\r";
                break;
            case '\t':
                buffer_ += "\\t";
                break;
            default:
                // Control characters have to be escaped; everything else, including
                // UTF-8 continuation bytes, goes through untouched.
                if (static_cast<unsigned char>(character) < 0x20) {
                    std::ostringstream escape_;
                    escape_ << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                            << static_cast<int>(static_cast<unsigned char>(character));
                    buffer_ += escape_.str();
                } else {
                    buffer_ += character;
                }
                break;
            }
        }
    }

    std::string buffer_;
    std::vector<bool> first_;
    int indentWidth_ = 0;
    std::size_t depth_ = 0;
    bool pendingValue_ = false;
};

} // namespace friction::detail

namespace friction {

inline std::string Report::toJson(int indentWidth) const {
    detail::JsonWriter json(indentWidth);
    json.beginObject();

    json.member("schema", "friction.report/1");
    json.member("hardwareClock", hardwareClock);
    json.member("clockFrequencyHz", clockFrequency);
    json.member("threadCount", static_cast<std::uint64_t>(threadCount));

    json.key("process");
    json.beginObject();
    json.member("supported", process.supported());
    json.member("elapsedMilliseconds", process.elapsedMilliseconds);
    json.member("residentBytes", process.residentBytes);
    json.member("peakResidentBytes", process.peakResidentBytes);
    json.member("residentGrowthBytes", process.residentGrowthBytes);
    json.member("minorFaults", process.minorFaults);
    json.member("majorFaults", process.majorFaults);
    json.member("voluntaryContextSwitches", process.voluntaryContextSwitches);
    json.member("involuntaryContextSwitches", process.involuntaryContextSwitches);
    json.member("userCpuMilliseconds", process.userCpuMilliseconds);
    json.member("systemCpuMilliseconds", process.systemCpuMilliseconds);
    json.member("cpuUtilizationPercent", process.cpuUtilizationPercent);
    json.member("threadCount", static_cast<std::uint64_t>(process.threadCount));
    json.endObject();

    json.key("zones");
    json.beginArray();
    for (const auto& zone : zones) {
        json.beginObject();
        json.member("name", zone.name);
        json.member("calls", zone.callCount);
        json.member("threads", static_cast<std::uint64_t>(zone.threadCount));
        json.member("selfPercent", zone.selfPercent);
        json.member("selfMilliseconds", zone.selfMilliseconds);
        json.member("inclusiveMilliseconds", zone.inclusiveMilliseconds);
        json.member("meanNanoseconds", zone.meanNanoseconds);
        json.member("standardDeviationNanoseconds", zone.standardDeviationNanoseconds);
        json.member("minNanoseconds", zone.minNanoseconds);
        json.member("p50Nanoseconds", zone.p50Nanoseconds);
        json.member("p90Nanoseconds", zone.p90Nanoseconds);
        json.member("p99Nanoseconds", zone.p99Nanoseconds);
        json.member("p999Nanoseconds", zone.p999Nanoseconds);
        json.member("maxNanoseconds", zone.maxNanoseconds);
        json.member("saturated", zone.saturated);
        json.endObject();
    }
    json.endArray();

    json.key("counters");
    json.beginArray();
    for (const auto& counter : counters) {
        json.beginObject();
        json.member("name", counter.name);
        json.member("value", counter.value);
        json.endObject();
    }
    json.endArray();

    json.endObject();
    return json.take();
}

inline std::string Report::toString() const {
    std::ostringstream out;
    out << std::fixed;

    out << "Friction report\n";
    out << "  session    " << detail::formatMilliseconds(process.elapsedMilliseconds) << " wall, "
        << threadCount << (threadCount == 1 ? " thread" : " threads") << ", clock "
        << (hardwareClock ? "TSC" : "steady_clock") << " at " << std::setprecision(3)
        << (static_cast<double>(clockFrequency) / 1.0e9) << " GHz\n";

    if (process.supported()) {
        out << "  process    rss " << detail::formatBytes(process.residentBytes) << " ("
            << detail::formatByteDelta(process.residentGrowthBytes) << " over the session), peak "
            << detail::formatBytes(process.peakResidentBytes) << '\n';
        out << "  faults     " << process.minorFaults << " minor, " << process.majorFaults << " major\n";
        out << "  switches   " << process.voluntaryContextSwitches << " voluntary, "
            << process.involuntaryContextSwitches << " involuntary, cpu " << std::setprecision(1)
            << process.cpuUtilizationPercent << "% of "
            << detail::formatMilliseconds(process.elapsedMilliseconds) << '\n';
    } else {
        out << "  process    unavailable on this platform\n";
    }

    if (!zones.empty()) {
        out << "\n  region                          self%        self      total    calls"
               "       mean         p50         p99         max\n";
        for (const auto& zone : zones) {
            out << "  " << std::left << std::setw(30) << zone.name << std::right << std::setw(6)
                << std::setprecision(1) << zone.selfPercent << '%' << std::setw(13)
                << detail::formatMilliseconds(zone.selfMilliseconds) << std::setw(13)
                << detail::formatMilliseconds(zone.inclusiveMilliseconds) << std::setw(9)
                << zone.callCount << std::setw(13)
                << detail::formatNanoseconds(zone.meanNanoseconds) << std::setw(13)
                << detail::formatNanoseconds(zone.p50Nanoseconds) << std::setw(13)
                << detail::formatNanoseconds(zone.p99Nanoseconds) << std::setw(13)
                << detail::formatNanoseconds(zone.maxNanoseconds) << '\n';
            if (zone.saturated) {
                out << "  " << std::string(30, ' ')
                    << "saturated: samples reached the top of the tracked range\n";
            }
        }
    }

    if (!counters.empty()) {
        out << "\n  counter                                                        value\n";
        for (const auto& counter : counters) {
            out << "  " << std::left << std::setw(60) << counter.name << std::right << std::setw(12)
                << counter.value << '\n';
        }
    }

    return out.str();
}

} // namespace friction


// ============ friction/detail/session_state.hpp ============

///
/// \file session_state.hpp
///
/// Whether anything is being measured right now.
///
/// One process-wide pointer, read on the hot path and written by whoever starts
/// or stops a session. A scope entered while it is null costs a single load and
/// a predictable branch, which is what lets the instrumentation stay in release
/// builds without being switched off.
///
///
#include <atomic>


namespace friction::detail {

/// Configuration shared with every thread taking part in a session.
struct SessionState {
    Histogram::Config histogram{};
    std::uint64_t frequency = 0;

    /// Whether the `Session` that owns this state is still recording.
    ///
    /// The flag lives here rather than in the session because a session that
    /// opens another one has to be able to switch the first off without holding a
    /// pointer to it. Whoever opened last clears the flag on the state it
    /// displaced.
    bool recording = false;
};

/// Storage for the process-wide session pointer.
[[nodiscard]] inline std::atomic<SessionState*>& current() noexcept {
    static std::atomic<SessionState*> pointer{nullptr};
    return pointer;
}

/// The session currently recording, or null. Written only by start and stop.
///
/// Atomic because one thread publishes the pointer while every other thread
/// reads it to decide whether to record. Acquire and release are the pairing
/// that makes the session's configuration -- its histogram geometry above all --
/// visible to a thread that observes a non-null pointer, and on the architectures
/// this library targets they compile to the same plain load and store a
/// non-atomic pointer would have produced.
[[nodiscard]] inline SessionState* activeSession() noexcept {
    return current().load(std::memory_order_acquire);
}

/// Publishes the session, or clears it with nullptr.
inline void setActiveSession(SessionState* session) noexcept {
    current().store(session, std::memory_order_release);
}

} // namespace friction::detail


// ============ friction/session.hpp ============

///
/// \file session.hpp
///
/// The lifetime of a measurement.
///
///     auto session = friction::Session::start();
///     runWorkload();
///     const friction::Report report = session.stop();
///
/// Starting a session marks the process as measuring, which is the single flag
/// every `FRICTION_ZONE` on every thread checks. Stopping it samples the
/// operating system's view of the process, merges the samples each thread
/// collected into one distribution per region, and returns plain values.
///
/// Start and stop are meant to be called from one thread with the work they
/// measure quiesced: a session left running keeps instrumented code on its slow
/// path, and stopping while scopes are still open loses those last samples.
///
///

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace friction {

/// Knobs that change what a session measures.
struct SessionConfig {
    /// Range and resolution of every histogram the session builds. The range is
    /// in clock ticks, and anything longer saturates rather than growing.
    Histogram::Config histogram{};

    /// Sample the operating system's process counters at start and stop.
    ///
    /// Each sample reads a couple of proc files. That is nothing next to a
    /// workload, but it is not something to pay twice a second in a hot loop.
    bool sampleProcess = true;
};

/// Aggregates the samples every thread collected while it was running.
class Session final {
public:
    /// Opens a session with default settings.
    [[nodiscard]] static Session start() { return Session(SessionConfig{}); }

    /// Opens a session with the given configuration.
    [[nodiscard]] static Session start(const SessionConfig& config) { return Session(config); }

    /// Opens a session.
    ///
    /// Opening a session while another is running stops the first one, whose
    /// report is then lost. One process-wide recording at a time is a constraint
    /// worth keeping: it is what lets a scope check a single pointer to decide
    /// whether it is measuring anything.
    explicit Session(const SessionConfig& config = SessionConfig{}) { begin(config); }

    ~Session() {
        if (state_.recording) {
            stop();
        }
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Session(Session&& other) noexcept
        : state_(other.state_), counters_(std::move(other.counters_)),
          startTicks_(std::exchange(other.startTicks_, 0)), startProcess_(other.startProcess_),
          config_(other.config_), lastReport_(std::move(other.lastReport_)) {
        // The global pointer was aimed at the source's state, which the move has
        // just replaced with a copy of itself. Aim it at the destination instead,
        // and leave the source knowing it no longer owns the recording.
        if (state_.recording) {
            detail::setActiveSession(&state_);
        }
        other.state_.recording = false;
    }

    Session& operator=(Session&&) = delete;

    /// Closes the session and returns what it measured.
    ///
    /// Safe to call more than once; later calls return the first report again.
    Report stop() {
        if (!state_.recording) {
            return lastReport_;
        }

        const ProcessStats endProcess = config_.sampleProcess ? readProcessStats() : ProcessStats{};
        const Clock::Ticks endTicks = Clock::now();

        // Switch the scopes off before reading anything, so threads that are
        // still running stop adding samples while the merge runs rather than
        // after it. Only clear the global pointer if it is still ours: a session
        // that was superseded has no business switching off its successor.
        state_.recording = false;
        if (detail::activeSession() == &state_) {
            detail::setActiveSession(nullptr);
        }

        lastReport_ = build(endTicks, endProcess);
        return lastReport_;
    }

    /// True until `stop()` runs, or until another session supersedes this one.
    [[nodiscard]] bool active() const noexcept { return state_.recording; }

    /// Wall time since the session opened, in milliseconds.
    [[nodiscard]] double elapsedMilliseconds() const noexcept {
        return Clock::toMilliseconds(Clock::now() - startTicks_, state_.frequency);
    }

    /// Counters for this session.
    ///
    /// Register names during setup, before the threads that will increment them
    /// start, and keep the reference: resolving a name allocates.
    [[nodiscard]] CounterSet& counters() noexcept { return counters_; }

    /// The report from `stop()`, or an empty one if the session is still running.
    [[nodiscard]] const Report& report() const noexcept { return lastReport_; }

private:
    /// One region's samples from every thread, waiting to be summarized.
    struct MergedZone {
        std::string name;
        Histogram inclusive;
        Histogram selfTime;
        std::size_t threadCount = 0;
    };

    void begin(const SessionConfig& config) {
        config_ = config;
        state_.histogram = config.histogram;
        state_.frequency = Clock::frequency();

        // Drop the previous session's samples, so a long-lived process that opens
        // many sessions does not accumulate histograms nobody will ever read.
        //
        // The geometry is reapplied here rather than left to whichever thread
        // registers first: a thread that measured under a previous session already
        // has a state, and without this the new configuration would silently
        // apply to some threads and not others.
        for (auto* node : detail::Registry::snapshot()) {
            node->state.reset(config.histogram);
        }

        startProcess_ = config.sampleProcess ? readProcessStats() : ProcessStats{};
        startTicks_ = Clock::now();

        // One process records at a time. The session being displaced learns it is
        // finished, so its destructor stays quiet and its report reads as empty
        // rather than reporting work that belongs to whoever replaced it.
        if (detail::SessionState* const previous = detail::activeSession(); previous != nullptr) {
            previous->recording = false;
        }

        state_.recording = true;
        detail::setActiveSession(&state_);
    }

    /// Merges every thread's samples into one report.
    [[nodiscard]] Report build(Clock::Ticks endTicks, const ProcessStats& endProcess) const {
        Report report;
        report.clockFrequency = state_.frequency;
        report.hardwareClock = Clock::isHardwareCounter();
        report.process = summarizeProcess(endProcess, endTicks);

        const auto threads = detail::Registry::snapshot();
        report.threadCount = threads.size();

        std::vector<MergedZone> merged;
        for (const auto* node : threads) {
            for (const auto& slot : node->state.zones()) {
                if (slot->inclusive.count() == 0) {
                    continue;
                }
                MergedZone& target = mergeInto(merged, slot->name, node->state.histogramConfig());
                target.inclusive.merge(slot->inclusive);
                target.selfTime.merge(slot->selfTime);
                ++target.threadCount;
            }
        }

        // Slowest first, so the top of the report answers "where did the time go".
        std::sort(merged.begin(), merged.end(), [](const MergedZone& left, const MergedZone& right) {
            return left.selfTime.sum() > right.selfTime.sum();
        });

        double totalSelfNanoseconds = 0.0;
        for (const auto& zone : merged) {
            totalSelfNanoseconds += Clock::toNanosecondsExact(zone.selfTime.sum(), state_.frequency);
        }

        report.zones.reserve(merged.size());
        for (auto& zone : merged) {
            report.zones.push_back(summarize(std::move(zone), totalSelfNanoseconds));
        }

        report.counters.reserve(counters_.size());
        for (std::size_t index = 0; index < counters_.size(); ++index) {
            report.counters.push_back(CounterReport{std::string(counters_.name(index)), counters_.value(index)});
        }

        return report;
    }

    /// Finds the accumulated samples for a region name, or starts a new set.
    [[nodiscard]] static MergedZone& mergeInto(std::vector<MergedZone>& merged, const std::string& name,
                                               const Histogram::Config& config) {
        for (auto& zone : merged) {
            if (zone.name == name) {
                return zone;
            }
        }
        merged.push_back(MergedZone{name, Histogram(config), Histogram(config), 0});
        return merged.back();
    }

    /// Converts one merged region from ticks into the units a report is read in.
    /// Every conversion happens here, once, so no hot path ever sees a double.
    [[nodiscard]] ZoneReport summarize(MergedZone zone, double totalSelfNanoseconds) const {
        const Clock::Ticks frequency = state_.frequency;
        const Histogram& inclusive = zone.inclusive;

        const double inclusiveNanoseconds = Clock::toNanosecondsExact(inclusive.sum(), frequency);
        const double selfNanoseconds = Clock::toNanosecondsExact(zone.selfTime.sum(), frequency);
        const auto calls = inclusive.count();

        ZoneReport report;
        report.name = std::move(zone.name);
        report.threadCount = zone.threadCount;
        report.callCount = calls;
        report.inclusiveMilliseconds = inclusiveNanoseconds / 1.0e6;
        report.selfMilliseconds = selfNanoseconds / 1.0e6;
        report.meanNanoseconds = calls > 0 ? inclusiveNanoseconds / static_cast<double>(calls) : 0.0;
        report.standardDeviationNanoseconds =
            Clock::toNanosecondsExact(static_cast<Clock::Ticks>(inclusive.standardDeviation()), frequency);
        report.minNanoseconds = Clock::toNanoseconds(inclusive.min(), frequency);
        report.p50Nanoseconds = Clock::toNanoseconds(inclusive.median(), frequency);
        report.p90Nanoseconds = Clock::toNanoseconds(inclusive.p90(), frequency);
        report.p99Nanoseconds = Clock::toNanoseconds(inclusive.p99(), frequency);
        report.p999Nanoseconds = Clock::toNanoseconds(inclusive.p999(), frequency);
        report.maxNanoseconds = Clock::toNanoseconds(inclusive.max(), frequency);
        report.meanCallsPerThread =
            zone.threadCount > 0 ? static_cast<double>(calls) / static_cast<double>(zone.threadCount) : 0.0;
        report.saturated = inclusive.saturated();
        if (totalSelfNanoseconds > 0.0) {
            report.selfPercent = selfNanoseconds / totalSelfNanoseconds * 100.0;
        }
        return report;
    }

    [[nodiscard]] ProcessReport summarizeProcess(const ProcessStats& end, Clock::Ticks endTicks) const {
        ProcessReport process;
        process.start = startProcess_;
        process.end = end;
        process.elapsedMilliseconds = Clock::toMilliseconds(endTicks - startTicks_, state_.frequency);

        if (process.start.source == ProcessSource::unknown) {
            return process;
        }

        // Counters only ever go up, so the unsigned subtraction below cannot
        // wrap on a system that reports them monotonically.
        process.residentBytes = end.residentBytes;
        process.peakResidentBytes = end.peakResidentBytes;
        process.residentGrowthBytes = static_cast<std::int64_t>(end.residentBytes) -
                                      static_cast<std::int64_t>(startProcess_.residentBytes);
        process.minorFaults = end.minorFaults - startProcess_.minorFaults;
        process.majorFaults = end.majorFaults - startProcess_.majorFaults;
        process.voluntaryContextSwitches = end.voluntaryContextSwitches - startProcess_.voluntaryContextSwitches;
        process.involuntaryContextSwitches =
            end.involuntaryContextSwitches - startProcess_.involuntaryContextSwitches;
        process.userCpuMilliseconds = (end.userCpuNanoseconds - startProcess_.userCpuNanoseconds) / 1'000'000;
        process.systemCpuMilliseconds =
            (end.systemCpuNanoseconds - startProcess_.systemCpuNanoseconds) / 1'000'000;
        process.threadCount = end.threadCount;

        if (process.elapsedMilliseconds > 0.0) {
            const auto cpuMilliseconds = static_cast<double>(process.userCpuMilliseconds) +
                                          static_cast<double>(process.systemCpuMilliseconds);
            process.cpuUtilizationPercent = cpuMilliseconds / process.elapsedMilliseconds * 100.0;
        }

        return process;
    }

    detail::SessionState state_{};
    CounterSet counters_;
    Clock::Ticks startTicks_ = 0;
    ProcessStats startProcess_{};
    SessionConfig config_{};
    Report lastReport_{};
};

} // namespace friction


// ============ friction/zone.hpp ============

///
/// \file zone.hpp
///
/// Timing scopes.
///
/// A scope is entered by the constructor and left by the destructor, so a
/// measurement cannot drift away from the code it wraps and cannot be left
/// unclosed by an early return or a thrown exception. Instrumenting a region is
/// a single line at the site you care about.
///
///     void parse(const std::string& text) {
///         FRICTION_ZONE("parse");
///         ...
///     }
///
/// While no session is recording, that same line costs one load of a global
/// pointer and a predictable branch. Nothing is timed and nothing is touched,
/// which is what makes it reasonable to leave instrumentation in release builds.
///
///

#include <string_view>
#include <utility>

namespace friction {

/// Times the region it is declared in and records it on the way out.
///
/// Movable, so a scope can be held in a container or handed to a function that
/// decides when to finish it. A moved-from scope records nothing.
class ScopedZone final {
public:
    ScopedZone() noexcept = default;

    ScopedZone(std::string_view name) noexcept { enter(name); }

    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;

    ScopedZone(ScopedZone&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)), start_(std::exchange(other.start_, 0)) {}

    ScopedZone& operator=(ScopedZone&&) = delete;

    ~ScopedZone() noexcept { close(); }

    /// True while this scope is actually recording.
    [[nodiscard]] bool active() const noexcept { return state_ != nullptr; }

    /// Ends the scope early. Calling it twice, or letting the destructor run
    /// afterwards, is harmless.
    void close() noexcept {
        if (state_ != nullptr) {
            state_->leave(Clock::now() - start_);
            state_ = nullptr;
        }
    }

private:
    void enter(std::string_view name) noexcept {
        const detail::SessionState* const session = detail::activeSession();
        if (FRICTION_UNLIKELY(session == nullptr)) {
            return;
        }
        state_ = &detail::Registry::local(session->histogram);
        state_->enter(&state_->zone(name));
        start_ = Clock::now();
    }

    detail::ThreadState* state_ = nullptr;
    Clock::Ticks start_ = 0;
};

} // namespace friction

#define FRICTION_DETAIL_ZONE_VARIABLE_2(line) friction_zone_##line
#define FRICTION_DETAIL_ZONE_VARIABLE(line) FRICTION_DETAIL_ZONE_VARIABLE_2(line)

/// Records the enclosing scope under a name.
///
/// The name should be a string literal. Friction keys regions on the address of
/// the literal, so passing one is what keeps the lookup a hash probe instead of
/// a string comparison.
///
/// The macro declares a variable, so the measurement covers the rest of the
/// enclosing block, not the rest of the statement. At the top of a function,
/// which is where instrumentation belongs, the two are the same thing:
///
///     void parseFrame(const Buffer& buffer) {
///         FRICTION_ZONE("parseFrame");
///         ...
///     }
///
/// Inside a loop or any other block you do not want to measure, wrap it:
///
///     for (const auto& request : queue) {
///         { FRICTION_ZONE("handleRequest"); handle(request); }
///     }
///
/// Getting this wrong is silent in one direction only: a zone left open when the
/// session stops loses its sample, and a zone closed early measures less than it
/// wrapped. Neither shows up as an error.
#define FRICTION_ZONE(name) ::friction::ScopedZone FRICTION_DETAIL_ZONE_VARIABLE(__LINE__) { name }


// ============ friction/friction.hpp ============

///
/// \file friction.hpp
///
/// Friction: low overhead performance measurement for C++ programs.
///
/// One header, no linking, no background threads. Include it, wrap the regions
/// you care about, read the report.
///
///     #include <friction/friction.hpp>
///
///     void parseFrame(const Buffer& buffer) {
///         FRICTION_ZONE("parseFrame");
///         ...
///     }
///
///     int main() {
///         auto session = friction::Session::start();
///         serveRequests();
///         const friction::Report report = session.stop();
///
///         std::cout << report.toString();
///         std::cout << report.toJson();
///     }
///
/// What it gives you:
///
///   - Regions, measured. Calls, wall time, and the distribution rather than
///     just the average, because the average is not what a user feels.
///   - Self time, so nested regions subtract and the top of the report answers
///     where the time actually went.
///   - Counters, for the things a timer cannot see.
///   - The operating system's own numbers for the process: memory, page faults,
///     context switches, CPU time and how many cores were actually busy.
///
/// What it costs while it runs: two counter reads per region, one increment per
/// exit, and nothing else. No locks, no allocation after the first visit to a
/// region, no work at all when no session is open.
///
/// The headers can be included individually. This one just names all of them.
///
///
/// The individual pieces, in the order a reader wants them:
///
///   - `friction/clock.hpp`      the tick source everything is measured with
///   - `friction/histogram.hpp`  bounded log-linear distributions
///   - `friction/counters.hpp`   sharded counters
///   - `friction/process.hpp`    operating system process counters
///   - `friction/zone.hpp`       `FRICTION_ZONE`
///   - `friction/report.hpp`     what a session produces
///   - `friction/session.hpp`    starting, stopping and aggregating
///


#define FRICTION_VERSION_MAJOR 0
#define FRICTION_VERSION_MINOR 1
#define FRICTION_VERSION_PATCH 0

namespace friction {

/// Version as a single comparable integer, 0xMMmmpp.
[[nodiscard]] constexpr unsigned version() noexcept {
    return (FRICTION_VERSION_MAJOR << 16) | (FRICTION_VERSION_MINOR << 8) | FRICTION_VERSION_PATCH;
}

/// Human readable version, "0.1.0".
[[nodiscard]] constexpr const char* versionString() noexcept { return "0.1.0"; }

} // namespace friction

