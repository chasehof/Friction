#pragma once

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
#include <friction/clock.hpp>
#include <friction/counters.hpp>
#include <friction/detail/registry.hpp>
#include <friction/detail/session_state.hpp>
#include <friction/detail/thread_state.hpp>
#include <friction/histogram.hpp>
#include <friction/process.hpp>
#include <friction/report.hpp>

#include <algorithm>
#include <cstddef>
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
    [[nodiscard]] static Session start() {
        return Session(SessionConfig{});
    }

    /// Opens a session with the given configuration.
    [[nodiscard]] static Session start(const SessionConfig& config) {
        return Session(config);
    }

    /// Opens a session.
    ///
    /// Opening a session while another is running stops the first one, whose
    /// report is then lost. One process-wide recording at a time is a constraint
    /// worth keeping: it is what lets a scope check a single pointer to decide
    /// whether it is measuring anything.
    explicit Session(const SessionConfig& config = SessionConfig{}) {
        begin(config);
    }

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
    [[nodiscard]] bool active() const noexcept {
        return state_.recording;
    }

    /// Wall time since the session opened, in milliseconds.
    [[nodiscard]] double elapsedMilliseconds() const noexcept {
        return Clock::toMilliseconds(Clock::now() - startTicks_, state_.frequency);
    }

    /// Counters for this session.
    ///
    /// Register names during setup, before the threads that will increment them
    /// start, and keep the reference: resolving a name allocates.
    [[nodiscard]] CounterSet& counters() noexcept {
        return counters_;
    }

    /// The report from `stop()`, or an empty one if the session is still running.
    [[nodiscard]] const Report& report() const noexcept {
        return lastReport_;
    }

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
        std::sort(merged.begin(), merged.end(),
                  [](const MergedZone& left, const MergedZone& right) {
                      return left.selfTime.sum() > right.selfTime.sum();
                  });

        double totalSelfNanoseconds = 0.0;
        for (const auto& zone : merged) {
            totalSelfNanoseconds +=
                Clock::toNanosecondsExact(zone.selfTime.sum(), state_.frequency);
        }

        report.zones.reserve(merged.size());
        for (auto& zone : merged) {
            report.zones.push_back(summarize(std::move(zone), totalSelfNanoseconds));
        }

        report.counters.reserve(counters_.size());
        for (std::size_t index = 0; index < counters_.size(); ++index) {
            report.counters.push_back(
                CounterReport{std::string(counters_.name(index)), counters_.value(index)});
        }

        return report;
    }

    /// Finds the accumulated samples for a region name, or starts a new set.
    [[nodiscard]] static MergedZone& mergeInto(std::vector<MergedZone>& merged,
                                               const std::string& name,
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
        report.meanNanoseconds =
            calls > 0 ? inclusiveNanoseconds / static_cast<double>(calls) : 0.0;
        report.standardDeviationNanoseconds = Clock::toNanosecondsExact(
            static_cast<Clock::Ticks>(inclusive.standardDeviation()), frequency);
        report.minNanoseconds = Clock::toNanoseconds(inclusive.min(), frequency);
        report.p50Nanoseconds = Clock::toNanoseconds(inclusive.median(), frequency);
        report.p90Nanoseconds = Clock::toNanoseconds(inclusive.p90(), frequency);
        report.p99Nanoseconds = Clock::toNanoseconds(inclusive.p99(), frequency);
        report.p999Nanoseconds = Clock::toNanoseconds(inclusive.p999(), frequency);
        report.maxNanoseconds = Clock::toNanoseconds(inclusive.max(), frequency);
        report.meanCallsPerThread = zone.threadCount > 0 ? static_cast<double>(calls) /
                                                               static_cast<double>(zone.threadCount)
                                                         : 0.0;
        report.saturated = inclusive.saturated();
        if (totalSelfNanoseconds > 0.0) {
            report.selfPercent = selfNanoseconds / totalSelfNanoseconds * 100.0;
        }
        return report;
    }

    [[nodiscard]] ProcessReport summarizeProcess(const ProcessStats& end,
                                                 Clock::Ticks endTicks) const {
        ProcessReport process;
        process.start = startProcess_;
        process.end = end;
        process.elapsedMilliseconds =
            Clock::toMilliseconds(endTicks - startTicks_, state_.frequency);

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
        process.voluntaryContextSwitches =
            end.voluntaryContextSwitches - startProcess_.voluntaryContextSwitches;
        process.involuntaryContextSwitches =
            end.involuntaryContextSwitches - startProcess_.involuntaryContextSwitches;
        process.userCpuMilliseconds =
            (end.userCpuNanoseconds - startProcess_.userCpuNanoseconds) / 1'000'000;
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
