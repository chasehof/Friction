#pragma once

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
#include <friction/clock.hpp>
#include <friction/counters.hpp>
#include <friction/process.hpp>

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