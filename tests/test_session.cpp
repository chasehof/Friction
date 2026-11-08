///
/// \file test_session.cpp
///
/// Tests for the measurement lifecycle: what gets recorded, what does not, and
/// how samples from several threads come back together.
///
#include "harness.hpp"

#include <friction/friction.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using friction::Report;
using friction::Session;
using friction::SessionConfig;

namespace {

/// A region that takes a predictable amount of time, so tests can assert on
/// ordering and nesting without depending on the machine's speed.
void busy(std::uint64_t iterations) {
    volatile std::uint64_t sink = 0;
    for (std::uint64_t index = 0; index < iterations; ++index) {
        sink = sink + index;
    }
}

} // namespace

FRICTION_TEST(session, records_a_region) {
    Session session = Session::start();
    for (int index = 0; index < 250; ++index) {
        FRICTION_ZONE("work");
        busy(100);
    }
    const Report report = session.stop();

    const auto* zone = report.find("work");
    CHECK(zone != nullptr);
    CHECK_EQ(zone->callCount, std::uint64_t{250});
    CHECK_EQ(zone->threadCount, std::size_t{1});
    CHECK(zone->inclusiveMilliseconds >= 0.0);
    CHECK(zone->maxNanoseconds >= zone->minNanoseconds);
    CHECK(zone->p99Nanoseconds <= zone->maxNanoseconds);
    CHECK_FALSE(zone->saturated);
}

FRICTION_TEST(session, records_nothing_when_closed) {
    Session first = Session::start();
    for (int index = 0; index < 100; ++index) {
        FRICTION_ZONE("measured");
    }
    first.stop();

    // No session is open here, so these are timed by nobody.
    for (int index = 0; index < 100; ++index) {
        FRICTION_ZONE("unmeasured");
    }

    // A session that opens later must not inherit them either: starting one
    // drops the previous session's samples, so work done outside any session
    // cannot leak into the next report.
    Session second = Session::start();
    const Report report = second.stop();

    CHECK(report.find("unmeasured") == nullptr);
    CHECK(report.find("measured") == nullptr);
    CHECK(report.zones.empty());
}

FRICTION_TEST(session, reports_nothing_while_no_session_is_open) {
    // The whole promise of leaving instrumentation in release builds: outside a
    // session, a scope is a load and a branch.
    CHECK(friction::detail::activeSession() == nullptr);

    for (int index = 0; index < 1000; ++index) {
        FRICTION_ZONE("nowhere");
    }

    CHECK(friction::detail::activeSession() == nullptr);
}

FRICTION_TEST(session, stopping_clears_the_active_flag) {
    Session session = Session::start();
    CHECK(session.active());
    CHECK(friction::detail::activeSession() != nullptr);
    session.stop();
    CHECK_FALSE(session.active());
    CHECK(friction::detail::activeSession() == nullptr);
}

FRICTION_TEST(session, stop_is_idempotent) {
    Session session = Session::start();
    {
        FRICTION_ZONE("once");
    }
    const Report first = session.stop();
    const Report second = session.stop();

    CHECK_EQ(first.find("once")->callCount, std::uint64_t{1});
    CHECK_EQ(second.find("once")->callCount, std::uint64_t{1});
}

FRICTION_TEST(session, nested_regions_subtract_from_their_parent) {
    Session session = Session::start();
    for (int index = 0; index < 50; ++index) {
        FRICTION_ZONE("parent");
        busy(50'000);
        FRICTION_ZONE("child");
        busy(400'000);
    }
    const Report report = session.stop();

    const auto* parent = report.find("parent");
    const auto* child = report.find("child");
    CHECK(parent != nullptr);
    CHECK(child != nullptr);

    // The parent encloses the child, so it must have spent at least as long.
    CHECK(parent->inclusiveMilliseconds >= child->inclusiveMilliseconds);
    // And its self time is what it spent without the child in it.
    CHECK(parent->selfMilliseconds < parent->inclusiveMilliseconds);
    CHECK(parent->selfMilliseconds > 0.0);
    CHECK(child->selfMilliseconds > 0.0);

    // The two regions together account for all of the session's self time.
    CHECK_NEAR(parent->selfPercent + child->selfPercent, 100.0, 0.001);
}

FRICTION_TEST(session, leaf_regions_measure_the_same_inclusive_and_self) {
    Session session = Session::start();
    for (int index = 0; index < 20; ++index) {
        FRICTION_ZONE("leaf");
        busy(10'000);
    }
    const Report report = session.stop();

    const auto* leaf = report.find("leaf");
    CHECK(leaf != nullptr);
    CHECK_NEAR(leaf->selfMilliseconds, leaf->inclusiveMilliseconds, 1.0e-12);
}

FRICTION_TEST(session, orders_regions_by_self_time) {
    Session session = Session::start();
    for (int index = 0; index < 10; ++index) {
        FRICTION_ZONE("quick");
        busy(10);
        FRICTION_ZONE("slow");
        busy(300'000);
    }
    const Report report = session.stop();

    CHECK(report.zones.size() >= 2);
    for (std::size_t index = 1; index < report.zones.size(); ++index) {
        CHECK(report.zones[index - 1].selfMilliseconds >= report.zones[index].selfMilliseconds);
    }
    CHECK_EQ(report.zones.front().name, std::string("slow"));
}

FRICTION_TEST(session, merges_samples_from_every_thread) {
    constexpr int kThreads = 6;
    constexpr int kIterationsPerThread = 500;

    Session session = Session::start();
    std::barrier gate{kThreads};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int index = 0; index < kThreads; ++index) {
        workers.emplace_back([&gate] {
            gate.arrive_and_wait();
            for (int iteration = 0; iteration < kIterationsPerThread; ++iteration) {
                FRICTION_ZONE("shared");
                busy(20);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    const Report report = session.stop();
    const auto* shared = report.find("shared");

    CHECK(shared != nullptr);
    CHECK_EQ(shared->callCount, std::uint64_t{kThreads * kIterationsPerThread});
    CHECK_EQ(shared->threadCount, std::size_t{kThreads});
    CHECK_NEAR(shared->meanCallsPerThread, double{kIterationsPerThread}, 1.0e-9);
    CHECK(report.threadCount >= kThreads);
}

FRICTION_TEST(session, keeps_threads_that_already_exited) {
    // Worker threads come and go, but their work is still worth reporting.
    Session session = Session::start();

    std::thread worker([] {
        for (int iteration = 0; iteration < 100; ++iteration) {
            FRICTION_ZONE("from_worker");
            busy(50);
        }
    });
    worker.join();

    for (int iteration = 0; iteration < 10; ++iteration) {
        FRICTION_ZONE("from_main");
        busy(50);
    }

    const Report report = session.stop();
    CHECK(report.find("from_worker") != nullptr);
    CHECK(report.find("from_main") != nullptr);
    CHECK_EQ(report.find("from_worker")->threadCount, std::size_t{1});
    CHECK_EQ(report.find("from_main")->threadCount, std::size_t{1});
}

FRICTION_TEST(session, carries_counters_into_the_report) {
    Session session = Session::start();
    auto& allocations = session.counters().counter("allocations");
    auto& bytes = session.counters().counter("bytes");
    CHECK_EQ(&allocations, &session.counters().counter("allocations"));

    for (int index = 0; index < 100; ++index) {
        allocations.add();
        bytes.add(64);
    }

    const Report report = session.stop();
    const auto* found = report.findCounter("allocations");
    CHECK(found != nullptr);
    CHECK_EQ(found->value, std::uint64_t{100});
    CHECK_EQ(report.findCounter("bytes")->value, std::uint64_t{6400});
    CHECK(report.findCounter("nonexistent") == nullptr);
    CHECK_EQ(report.counters.size(), std::size_t{2});
}

FRICTION_TEST(session, starts_a_fresh_measurement_each_time) {
    Session first = Session::start();
    for (int index = 0; index < 40; ++index) {
        FRICTION_ZONE("repeated");
    }
    CHECK_EQ(first.stop().find("repeated")->callCount, std::uint64_t{40});

    Session second = Session::start();
    for (int index = 0; index < 7; ++index) {
        FRICTION_ZONE("repeated");
    }
    CHECK_EQ(second.stop().find("repeated")->callCount, std::uint64_t{7});
}

FRICTION_TEST(session, a_new_session_supersedes_the_old_one) {
    Session first = Session::start();
    Session second = Session::start();

    {
        FRICTION_ZONE("after_the_second");
    }
    const Report report = second.stop();

    CHECK_FALSE(first.active());
    CHECK(report.find("after_the_second") != nullptr);
}

FRICTION_TEST(session, stopping_on_destruction) {
    // A session that goes out of scope must leave instrumentation switched off,
    // or every later region in the process keeps paying for a session nobody can
    // read.
    {
        Session session = Session::start();
        FRICTION_ZONE("scoped");
        CHECK(session.active());
    }
    CHECK(friction::detail::activeSession() == nullptr);

    {
        FRICTION_ZONE("after_destruction");
    }
    Session third = Session::start();
    const Report report = third.stop();
    CHECK(report.find("scoped") == nullptr);
    CHECK(report.find("after_destruction") == nullptr);
}

FRICTION_TEST(session, survives_a_scope_that_never_closes) {
    // An exception unwinds through a scope, so the destructor has to be the
    // thing that records the sample.
    Session session = Session::start();
    try {
        FRICTION_ZONE("throws");
        throw 42;
    } catch (int) {
    }

    const Report report = session.stop();
    CHECK(report.find("throws") != nullptr);
    CHECK_EQ(report.find("throws")->callCount, std::uint64_t{1});
}

FRICTION_TEST(session, honours_a_custom_histogram_range) {
    SessionConfig config;
    config.histogram.highestTrackableValue = 1000;
    config.histogram.significantFigures = 3;

    Session session = Session::start(config);
    {
        FRICTION_ZONE("configured");
        busy(20'000);
    }
    const Report report = session.stop();

    const auto* zone = report.find("configured");
    CHECK(zone != nullptr);
    CHECK(zone->saturated);
}

FRICTION_TEST(session, can_skip_process_sampling) {
    SessionConfig config;
    config.sampleProcess = false;

    Session session = Session::start(config);
    {
        FRICTION_ZONE("unmeasured_process");
    }
    const Report report = session.stop();

    CHECK_FALSE(report.process.supported());
    CHECK(report.find("unmeasured_process") != nullptr);
}

FRICTION_TEST(session, an_empty_session_is_not_an_error) {
    Session session = Session::start();
    const Report report = session.stop();

    CHECK(report.empty());
    CHECK(report.zones.empty());
    CHECK(report.clockFrequency > 0);
    CHECK(report.process.elapsedMilliseconds >= 0.0);
}

FRICTION_TEST(session, nested_scopes_beyond_the_depth_cap_are_still_counted) {
    Session session = Session::start();
    {
        FRICTION_ZONE("deep_0");
        FRICTION_ZONE("deep_1");
        FRICTION_ZONE("deep_2");
        FRICTION_ZONE("deep_3");
        FRICTION_ZONE("deep_4");
    }
    const Report report = session.stop();

    for (int index = 0; index < 5; ++index) {
        CHECK(report.find("deep_" + std::to_string(index)) != nullptr);
    }
    CHECK_NEAR(report.find("deep_0")->inclusiveMilliseconds,
               report.find("deep_4")->inclusiveMilliseconds, 0.5);
}

FRICTION_TEST(session, close_ends_a_scope_early) {
    Session session = Session::start();
    {
        friction::ScopedZone zone("closed_early");
        CHECK(zone.active());
        zone.close();
        CHECK_FALSE(zone.active());
        zone.close(); // must be harmless
    }
    const Report report = session.stop();
    CHECK_EQ(report.find("closed_early")->callCount, std::uint64_t{1});
}

FRICTION_TEST(session, a_scope_can_be_moved) {
    Session session = Session::start();
    {
        friction::ScopedZone original("moved");
        friction::ScopedZone taken = std::move(original);
        CHECK(taken.active());
        CHECK_FALSE(original.active());
    }
    const Report report = session.stop();
    CHECK_EQ(report.find("moved")->callCount, std::uint64_t{1});
}

FRICTION_TEST(session, one_name_from_two_call_sites_is_one_region) {
    // Region lookup keys on the address of the name literal, so a repeated
    // spelling at a different call site is a different address. It still has to
    // land in the same region, or the report would split one logical region in
    // two and every caller that reused a name would pay a text comparison.
    const char first[] = "shared.region";
    const char second[] = "shared.region";
    CHECK(std::string_view(first).data() != std::string_view(second).data());

    Session session = Session::start();
    {
        FRICTION_ZONE(first);
    }
    {
        FRICTION_ZONE(second);
    }
    {
        FRICTION_ZONE(first);
    }
    const Report report = session.stop();

    CHECK_EQ(report.zones.size(), std::size_t{1});
    const auto* zone = report.find("shared.region");
    CHECK(zone != nullptr);
    CHECK_EQ(zone->callCount, std::uint64_t{3});
}

FRICTION_TEST(session, more_regions_than_the_lookup_table_still_record) {
    // The per-thread lookup table is finite. Once it is full, new regions have to
    // fall back to the linear scan rather than overwrite an existing entry or
    // drop the sample.
    constexpr std::size_t kRegions = 200; // deliberately more than the table holds
    std::array<std::string, kRegions> names;
    for (std::size_t index = 0; index < names.size(); ++index) {
        names[index] = "region." + std::to_string(index);
    }

    Session session = Session::start();
    for (const auto& name : names) {
        friction::ScopedZone zone(name);
    }
    const Report report = session.stop();

    CHECK_EQ(report.zones.size(), kRegions);
    for (const auto& name : names) {
        const auto* zone = report.find(name);
        CHECK(zone != nullptr);
        if (zone != nullptr) {
            CHECK_EQ(zone->callCount, std::uint64_t{1});
        }
    }
}

FRICTION_TEST(session, elapsed_time_advances_and_is_reported_at_stop) {
    Session session = Session::start();
    const double before = session.elapsedMilliseconds();
    busy(2'000'000);
    const double after = session.elapsedMilliseconds();

    CHECK(after >= before);
    CHECK(after > 0.0);

    const Report report = session.stop();
    CHECK(report.process.elapsedMilliseconds > 0.0);
}

FRICTION_TEST(session, produces_stable_timings) {
    SKIP_IF_SANITIZED("timing assertions are meaningless under a sanitizer");
    // Sanity check that a measured region reports something ordered, without
    // pinning a machine-specific number into the test suite.
    Session session = Session::start();
    for (int index = 0; index < 100; ++index) {
        FRICTION_ZONE("small");
        busy(200);
    }
    for (int index = 0; index < 100; ++index) {
        FRICTION_ZONE("large");
        busy(20'000);
    }
    const Report report = session.stop();

    const double small = report.find("small")->meanNanoseconds;
    const double large = report.find("large")->meanNanoseconds;
    CHECK(large > small);
    CHECK(small > 0.0);
}
