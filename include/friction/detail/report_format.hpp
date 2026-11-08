#pragma once

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
#include <friction/report.hpp>

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