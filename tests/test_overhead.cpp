///
/// \file test_overhead.cpp
///
/// The numbers the library is sold on.
///
/// These measure the cost of being instrumented rather than of doing any work,
/// and assert loose ceilings. A ceiling loose enough to survive a loaded build
/// machine still fails the moment a mutex, an allocation or a syscall appears on
/// the measurement path, which is the regression worth catching.
///
#include "harness.hpp"

#include <friction/friction.hpp>

#include <algorithm>
#include <vector>

using friction::Clock;
using friction::Report;
using friction::Session;

namespace {

/// Ceilings for a modern x86 core. Deliberately loose; the point is to catch an
/// order of magnitude, not to pin a machine.
constexpr double kZoneCeilingNanoseconds = 150.0;
constexpr double kCounterCeilingNanoseconds = 150.0;
constexpr double kDisabledZoneCeilingNanoseconds = 5.0;
constexpr double kCounterSetLookupCeilingNanoseconds = 5000.0;

constexpr int kIterations = 1'000'000;

/// Average nanoseconds per iteration of `body`.
template <typename Body>
double averageNanosecondsPer(Body&& body) {
    const Clock::Ticks frequency = Clock::frequency();
    const Clock::Ticks start = Clock::now();
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        body();
    }
    const Clock::Ticks elapsed = Clock::now() - start;
    return Clock::toNanosecondsExact(elapsed, frequency) / kIterations;
}

} // namespace

FRICTION_TEST(overhead, a_scope_costs_nanoseconds) {
    SKIP_IF_SANITIZED("timing assertions are meaningless under a sanitizer");
    Session session = Session::start();

    // The first visit to a region allocates its bookkeeping, so warm it up before
    // measuring: this is the steady state cost, not the one-off setup.
    {
        FRICTION_ZONE("overhead.zone");
    }

    const double perScope = averageNanosecondsPer([] { FRICTION_ZONE("overhead.zone"); });

    session.stop();

    std::printf("    scope enter and leave      %6.1f ns\n", perScope);
    CHECK(perScope < kZoneCeilingNanoseconds);
}

FRICTION_TEST(overhead, an_empty_scope_does_not_swamp_the_work_it_wraps) {
    SKIP_IF_SANITIZED("timing assertions are meaningless under a sanitizer");
    // An empty scope is not the interesting case: two clock reads cost about what
    // they cost whatever they wrap. The claim worth testing is the one a user
    // actually relies on -- that a scope around real work leaves the work
    // measurable rather than distorted -- so this measures the distortion
    // directly instead of comparing against a hand-picked constant.
    Session session = Session::start();
    {
        FRICTION_ZONE("overhead.wrapped");
    }

    // A few hundred nanoseconds of arithmetic, about where a real leaf function
    // lands. The result has to escape into a volatile, or the compiler folds the
    // whole loop away and the baseline is measured as zero.
    volatile double sink = 0.0;
    auto burn = [&sink] {
        double accumulator = 1.0;
        for (int index = 1; index < 200; ++index) {
            accumulator += 1.0 / static_cast<double>(index);
        }
        sink = accumulator;
    };

    const double perBare = averageNanosecondsPer(burn);
    const double perScoped = averageNanosecondsPer([&] {
        FRICTION_ZONE("overhead.wrapped");
        burn();
    });
    session.stop();

    const double perScope = perScoped - perBare;
    const double distortionPercent = perBare > 0.0 ? perScope / perBare * 100.0 : 0.0;

    std::printf("    bare body                  %6.1f ns\n", perBare);
    std::printf("    the same body, scoped      %6.1f ns\n", perScoped);
    std::printf("    added by the scope         %6.1f ns  (%.1f%%)\n", perScope, distortionPercent);

    CHECK(perBare > 0.0);
    CHECK(perScope < kZoneCeilingNanoseconds);

    // Bounded in both directions. Two loops of a million iterations differ by a
    // fraction of a percent for reasons that have nothing to do with the scope --
    // code layout, branch prediction, frequency drift -- so the lower bound is not
    // zero. But a measurement that claims the scope made the body *faster* is
    // reporting its own noise, and should not be allowed to pass as a result.
    CHECK(perScoped >= perBare * 0.9);
    CHECK(perScoped <= perBare * 1.5);
}

FRICTION_TEST(overhead, nested_scopes_do_not_get_much_more_expensive) {
    SKIP_IF_SANITIZED("timing assertions are meaningless under a sanitizer");
    Session session = Session::start();
    {
        FRICTION_ZONE("overhead.outer");
    }

    const double perOuter = averageNanosecondsPer([] { FRICTION_ZONE("overhead.outer"); });
    const double perNestedPair = averageNanosecondsPer([] {
        FRICTION_ZONE("overhead.outer");
        FRICTION_ZONE("overhead.outer");
    });
    session.stop();

    const double perInner = perNestedPair - perOuter;
    std::printf("    nested inner scope         %6.1f ns\n", perInner);
    CHECK(perInner < kZoneCeilingNanoseconds);
}

FRICTION_TEST(overhead, a_counter_increment_costs_nanoseconds) {
    SKIP_IF_SANITIZED("timing assertions are meaningless under a sanitizer");
    Session session = Session::start();
    auto& counter = session.counters().counter("overhead.counter");

    const double perIncrement = averageNanosecondsPer([&counter] { counter.add(); });
    session.stop();

    std::printf("    counter increment          %6.1f ns\n", perIncrement);
    CHECK_EQ(counter.value(), std::uint64_t{kIterations});
    CHECK(perIncrement < kCounterCeilingNanoseconds);
}

FRICTION_TEST(overhead, resolving_a_counter_name_is_not_on_the_hot_path) {
    SKIP_IF_SANITIZED("timing assertions are meaningless under a sanitizer");
    // Documented as setup work, and priced accordingly: this measures the cost of
    // the mistake of looking a name up inside a loop, which is exactly why the
    // library hands out a reference to keep instead.
    friction::CounterSet counters;
    auto& warm = counters.counter("overhead.lookup");
    warm.add();

    const double perLookup =
        averageNanosecondsPer([&counters] { (void)counters.counter("overhead.lookup"); });

    std::printf("    counter name lookup        %6.1f ns\n", perLookup);
    CHECK(perLookup < kCounterSetLookupCeilingNanoseconds);
}

FRICTION_TEST(overhead, instrumentation_left_in_the_build_is_almost_free) {
    SKIP_IF_SANITIZED("timing assertions are meaningless under a sanitizer");
    // The reason a scope can stay in a release build: with no session open it
    // costs a load and a branch. This is the number that has to stay negligible.
    CHECK(friction::detail::activeSession() == nullptr);

    const double perScope = averageNanosecondsPer([] { FRICTION_ZONE("overhead.disabled"); });

    std::printf("    scope with no session      %6.1f ns\n", perScope);
    CHECK(perScope < kDisabledZoneCeilingNanoseconds);
}

FRICTION_TEST(overhead, reporting_does_not_scale_with_the_number_of_samples) {
    SKIP_IF_SANITIZED("timing assertions are meaningless under a sanitizer");
    // Recording is O(1) per sample, so a million samples have to cost about what
    // a thousand did. Anything that stored every sample would show up here.
    Session session = Session::start();

    const auto frequency = Clock::frequency();
    const Clock::Ticks shortStart = Clock::now();
    for (int iteration = 0; iteration < 1'000; ++iteration) {
        FRICTION_ZONE("overhead.scale");
    }
    const double perShort =
        Clock::toNanosecondsExact(Clock::now() - shortStart, frequency) / 1'000.0;

    const Clock::Ticks longStart = Clock::now();
    for (int iteration = 0; iteration < 100'000; ++iteration) {
        FRICTION_ZONE("overhead.scale");
    }
    const double perLong =
        Clock::toNanosecondsExact(Clock::now() - longStart, frequency) / 100'000.0;

    const Report report = session.stop();

    std::printf("    first 1k samples           %6.1f ns\n", perShort);
    std::printf("    next 100k samples          %6.1f ns\n", perLong);
    CHECK_EQ(report.find("overhead.scale")->callCount, std::uint64_t{101'000});
    CHECK(perLong < perShort * 4);
}

FRICTION_TEST(overhead, memory_stays_bounded_as_samples_accumulate) {
    // A histogram that grew with the data would be a memory leak wearing a
    // profiler's clothes, so its footprint has to be identical before and after
    // a quarter of a million samples.
    friction::Histogram histogram;
    const std::size_t before = histogram.memoryUsage();

    Session session = Session::start();
    for (int iteration = 0; iteration < 200'000; ++iteration) {
        FRICTION_ZONE("overhead.memory");
    }
    const Report report = session.stop();

    for (int iteration = 0; iteration < 200'000; ++iteration) {
        histogram.record(1000 + static_cast<std::uint64_t>(iteration));
    }

    std::printf("    histogram before samples   %6zu bytes\n", before);
    std::printf("    histogram after 200k       %6zu bytes\n", histogram.memoryUsage());
    CHECK_EQ(histogram.memoryUsage(), before);
    CHECK_EQ(histogram.count(), std::uint64_t{200'000});
    CHECK_EQ(report.find("overhead.memory")->callCount, std::uint64_t{200'000});
}
