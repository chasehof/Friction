#pragma once

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

#include <friction/detail/platform.hpp>

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