///
/// \file test_process.cpp
///
/// Tests for the operating system counters.
///
/// These read real kernel state, so they assert on relationships that have to
/// hold on any machine rather than on exact numbers, which would be a test that
/// fails on someone else's computer.
///
#include "harness.hpp"

#include <friction/friction.hpp>
#include <thread>

using friction::ProcessSource;
using friction::ProcessStats;

FRICTION_TEST(process, reads_something_real) {
    const ProcessStats stats = friction::readProcessStats();

#if defined(FRICTION_OS_LINUX) || defined(FRICTION_OS_APPLE)
    CHECK(friction::processStatsSupported());
    CHECK(stats.source != ProcessSource::unknown);
    CHECK(stats.threadCount >= 1);
#else
    CHECK_FALSE(friction::processStatsSupported());
    CHECK(stats.source == ProcessSource::unknown);
#endif
}

FRICTION_TEST(process, reports_memory_when_the_platform_has_it) {
    const ProcessStats stats = friction::readProcessStats();
#if defined(FRICTION_OS_LINUX)
    CHECK(stats.source == ProcessSource::procfs);
    CHECK(stats.hasResidentMemory());
    // A running process has pages resident, and a mapped address space.
    CHECK(stats.residentBytes > 0);
    CHECK(stats.virtualBytes > 0);
    // The high water mark cannot be below the current reading.
    CHECK(stats.peakResidentBytes >= stats.residentBytes);
#else
    CHECK_FALSE(stats.hasResidentMemory());
    CHECK_EQ(stats.residentBytes, std::uint64_t{0});
#endif
}

FRICTION_TEST(process, reports_cpu_time_when_the_platform_has_it) {
    const ProcessStats stats = friction::readProcessStats();
#if defined(FRICTION_OS_LINUX) || defined(FRICTION_OS_APPLE)
    // Some user time has certainly been spent getting this far.
    CHECK(stats.totalCpuNanoseconds() > 0);
    CHECK(stats.minorFaults > 0);
#else
    CHECK_EQ(stats.totalCpuNanoseconds(), std::uint64_t{0});
#endif
}

FRICTION_TEST(process, cpu_time_only_moves_forward) {
    const ProcessStats before = friction::readProcessStats();
    volatile std::uint64_t sink = 0;
    for (int index = 0; index < 2'000'000; ++index) {
        sink += static_cast<std::uint64_t>(index);
    }
    const ProcessStats after = friction::readProcessStats();

    CHECK(after.userCpuNanoseconds >= before.userCpuNanoseconds);
    CHECK(after.systemCpuNanoseconds >= before.systemCpuNanoseconds);
    CHECK(after.minorFaults >= before.minorFaults);
    CHECK(after.majorFaults >= before.majorFaults);
}

FRICTION_TEST(process, counts_the_threads_it_has) {
    const ProcessStats before = friction::readProcessStats();

    std::thread worker([] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); });

    // The count is read while the worker is alive.
    const ProcessStats during = friction::readProcessStats();
    worker.join();

#if defined(FRICTION_OS_LINUX)
    CHECK(during.threadCount >= before.threadCount + 1);
#else
    CHECK(during.threadCount >= before.threadCount);
#endif
}

FRICTION_TEST(process, handles_a_short_file_without_reading_past_it) {
#if defined(FRICTION_OS_LINUX)
    // A nonexistent path must fail cleanly rather than parse a stale buffer.
    friction::detail::ProcFile missing;
    CHECK_FALSE(missing.open("/proc/self/definitely-not-here"));
    std::uint64_t value = 0;
    CHECK_FALSE(missing.findUnsigned("VmRSS:", value));

    friction::detail::ProcFile file;
    CHECK(file.open("/proc/self/status"));
    CHECK(file.findUnsigned("voluntary_ctxt_switches:", value));
    CHECK_FALSE(file.findUnsigned("no_such_field:", value));
    CHECK_FALSE(file.readStatField(2, value)); // the command name is not a number
    CHECK(file.readStatField(20, value));
    CHECK(value >= 1);
#endif
}
