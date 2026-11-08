///
/// \file test_clock.cpp
///
/// Tests for the tick source everything else is measured with.
///
#include "harness.hpp"

#include <friction/friction.hpp>
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

using friction::Clock;

FRICTION_TEST(clock, is_monotonic) {
    Clock::Ticks previous = Clock::now();
    for (int index = 0; index < 100'000; ++index) {
        const Clock::Ticks current = Clock::now();
        if (current < previous) {
            CHECK_EQ(current, previous);
        }
        previous = current;
    }
    CHECK(true);
}

FRICTION_TEST(clock, frequency_is_plausible) {
    const Clock::Ticks frequency = Clock::frequency();
    CHECK(frequency > 1'000'000ull);
    // Fastest anything real runs, and slowest a calibrated value could come out
    // before rounding. Outside this range the calibration did not work.
    CHECK(frequency < 100'000'000'000ull);
}

FRICTION_TEST(clock, advances_in_real_time) {
    SKIP_IF_SANITIZED("timing assertions are meaningless under a sanitizer");
    // Cross-check the tick rate against the wall clock over a long enough window
    // that the two disagree by less than the tolerance if both are right.
    constexpr auto kDuration = std::chrono::milliseconds(50);
    const Clock::Ticks frequency = Clock::frequency();

    const auto wallStart = std::chrono::steady_clock::now();
    const Clock::Ticks tickStart = Clock::now();
    std::this_thread::sleep_for(kDuration);
    const Clock::Ticks tickEnd = Clock::now();
    const auto wallEnd = std::chrono::steady_clock::now();

    const double wallNanoseconds =
        std::chrono::duration<double, std::nano>(wallEnd - wallStart).count();
    const double tickNanoseconds = Clock::toNanosecondsExact(tickEnd - tickStart, frequency);
    const double ratio = tickNanoseconds / wallNanoseconds;

    std::printf("    measured %.6f ns of ticks per ns of wall clock\n", ratio);
    CHECK_NEAR(ratio, 1.0, 0.01);
}

FRICTION_TEST(clock, converts_a_second_correctly) {
    const Clock::Ticks frequency = Clock::frequency();
    CHECK_EQ(Clock::toNanoseconds(frequency, frequency), 1'000'000'000ull);
    CHECK_EQ(Clock::toNanoseconds(2 * frequency, frequency), 2'000'000'000ull);
    CHECK_EQ(Clock::toNanoseconds(0, frequency), 0ull);
    CHECK_NEAR(Clock::toMilliseconds(frequency, frequency), 1000.0, 1.0e-9);
}

FRICTION_TEST(clock, refuses_to_convert_without_a_rate) {
    // A zero frequency would otherwise divide by zero.
    CHECK_EQ(Clock::toNanoseconds(1234, 0), 0ull);
    CHECK_NEAR(Clock::toNanosecondsExact(1234, 0), 0.0, 1.0e-12);
}

FRICTION_TEST(clock, tick_conversion_keeps_precision) {
    const Clock::Ticks frequency = Clock::frequency();
    // Half a tick is well under a nanosecond and must not round away to nothing
    // over the conversions a report performs.
    const std::uint64_t converted = Clock::toNanoseconds(frequency / 2, frequency);
    CHECK_NEAR(static_cast<double>(converted), 5.0e8, 1.0e4);
}

FRICTION_TEST(clock, is_consistent_across_threads) {
    SKIP_IF_SANITIZED("timing assertions are meaningless under a sanitizer");
    // Every core has to be reading the same counter, or multi-threaded samples
    // would not be comparable. Comparing the readings against each other would
    // only measure how long the scheduler took to run four threads, so instead
    // each thread brackets its own tick reading between two wall-clock readings
    // and checks that the tick reading lands inside the interval that brackets
    // it. That fails if any core's counter runs at a different rate, is offset,
    // or moves backwards.
    constexpr int kThreads = 4;
    constexpr double kToleranceNanoseconds = 500.0;

    struct Observation {
        Clock::Ticks beforeTicks = 0;
        std::chrono::steady_clock::time_point beforeWall{};
        Clock::Ticks afterTicks = 0;
        std::chrono::steady_clock::time_point afterWall{};
    };

    std::vector<Observation> observations(kThreads);
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int index = 0; index < kThreads; ++index) {
        workers.emplace_back([&observations, index] {
            Observation& observation = observations[static_cast<std::size_t>(index)];
            observation.beforeWall = std::chrono::steady_clock::now();
            observation.beforeTicks = Clock::now();
            observation.afterTicks = Clock::now();
            observation.afterWall = std::chrono::steady_clock::now();
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    const double frequency = static_cast<double>(Clock::frequency());
    for (const Observation& observation : observations) {
        CHECK(observation.afterTicks >= observation.beforeTicks);

        const double wallBefore =
            std::chrono::duration<double, std::nano>(observation.beforeWall.time_since_epoch())
                .count();
        const double wallAfter =
            std::chrono::duration<double, std::nano>(observation.afterWall.time_since_epoch())
                .count();

        const double tickBefore = static_cast<double>(observation.beforeTicks) / frequency;
        const double tickAfter = static_cast<double>(observation.afterTicks) / frequency;

        // Absolute epoch values differ between the two clocks by an arbitrary
        // constant, so compare the interval each one measured rather than their
        // origins.
        const double wallSpan = wallAfter - wallBefore;
        const double tickSpan = tickAfter - tickBefore;
        CHECK(std::abs(tickSpan - wallSpan) < kToleranceNanoseconds);
        CHECK(tickBefore < tickAfter + kToleranceNanoseconds);
    }
}
