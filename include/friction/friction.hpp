#pragma once

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

#include <friction/clock.hpp>
#include <friction/counters.hpp>
#include <friction/histogram.hpp>
#include <friction/process.hpp>
#include <friction/report.hpp>
#include <friction/session.hpp>
#include <friction/zone.hpp>

#define FRICTION_VERSION_MAJOR 0
#define FRICTION_VERSION_MINOR 1
#define FRICTION_VERSION_PATCH 0

namespace friction {

/// Version as a single comparable integer, 0xMMmmpp.
[[nodiscard]] constexpr unsigned version() noexcept {
    return (FRICTION_VERSION_MAJOR << 16) | (FRICTION_VERSION_MINOR << 8) | FRICTION_VERSION_PATCH;
}

/// Human readable version, "0.1.0".
[[nodiscard]] constexpr const char* versionString() noexcept {
    return "0.1.0";
}

} // namespace friction
