///
/// \file test_report.cpp
///
/// Tests for what a finished session serializes to.
///
/// The JSON is the machine-readable half of the library's output, so the
/// invariants worth protecting are that every field is present, that strings are
/// escaped rather than injected, and that a non-finite number becomes null
/// instead of invalid JSON.
///
#include "harness.hpp"

#include <friction/friction.hpp>

#include <algorithm>
#include <locale>
#include <numeric>
#include <string_view>

using friction::Clock;
using friction::Report;
using friction::Session;

namespace {

Report buildSampleReport() {
    Session session = Session::start();
    auto& operations = session.counters().counter("operations");
    operations.add(1234);

    for (int index = 0; index < 25; ++index) {
        FRICTION_ZONE("outer");
        FRICTION_ZONE("inner");
    }
    {
        FRICTION_ZONE("quoted \"name\"");
    }

    return session.stop();
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

/// Punctuation that would turn a JSON number into two values if the writer used
/// the process locale.
struct CommaDecimalPunctuation : std::numpunct<char> {
    char do_decimal_point() const override {
        return ',';
    }
    char do_thousands_sep() const override {
        return '.';
    }
    std::string do_grouping() const override {
        return "\3";
    }
};

/// Installs a global locale for the duration of a test and restores the one that
/// was there before.
class ScopedGlobalLocale final {
    public:
    explicit ScopedGlobalLocale(const std::locale& locale)
        : previous_(std::locale::global(locale)) {
    }
    ~ScopedGlobalLocale() {
        std::locale::global(previous_);
    }

    ScopedGlobalLocale(const ScopedGlobalLocale&) = delete;
    ScopedGlobalLocale& operator=(const ScopedGlobalLocale&) = delete;

    private:
    std::locale previous_;
};

} // namespace

FRICTION_TEST(report, renders_every_region) {
    const Report report = buildSampleReport();
    const std::string text = report.toString();

    CHECK(contains(text, "outer"));
    CHECK(contains(text, "inner"));
    CHECK(contains(text, "region"));
    CHECK(contains(text, "Friction report"));
    CHECK(contains(text, "session"));
}

FRICTION_TEST(report, renders_counters) {
    const Report report = buildSampleReport();
    const std::string text = report.toString();

    CHECK(contains(text, "operations"));
    CHECK(contains(text, "1234"));
}

FRICTION_TEST(report, json_has_the_expected_shape) {
    const Report report = buildSampleReport();
    const std::string json = report.toJson();

    CHECK(contains(json, "\"schema\": \"friction.report/1\""));
    CHECK(contains(json, "\"zones\": ["));
    CHECK(contains(json, "\"counters\": ["));
    CHECK(contains(json, "\"process\": {"));
    CHECK(contains(json, "\"p99Nanoseconds\""));
    CHECK(contains(json, "\"selfPercent\""));
    CHECK(contains(json, "\"clockFrequencyHz\""));
    CHECK(contains(json, "\"hardwareClock\""));
    CHECK(contains(json, "\"name\": \"outer\""));

    // Balanced braces and brackets, no more, no less.
    int braces = 0;
    int brackets = 0;
    for (const char character : json) {
        if (character == '{') {
            ++braces;
        } else if (character == '}') {
            --braces;
        } else if (character == '[') {
            ++brackets;
        } else if (character == ']') {
            --brackets;
        }
    }
    CHECK_EQ(braces, 0);
    CHECK_EQ(brackets, 0);
}

FRICTION_TEST(report, json_is_valid_without_indentation) {
    const Report report = buildSampleReport();
    const std::string json = report.toJson(0);
    CHECK_FALSE(contains(json, "\n"));
    CHECK(contains(json, "\"zones\": [{"));
}

FRICTION_TEST(report, json_escapes_names) {
    const Report report = buildSampleReport();
    const std::string json = report.toJson();

    // The name carries a quote, so it has to arrive escaped rather than closing
    // the string and corrupting the document.
    CHECK(contains(json, "quoted \\\"name\\\""));
}

FRICTION_TEST(report, json_numbers_are_finite) {
    const Report report = buildSampleReport();
    const std::string json = report.toJson();
    CHECK_FALSE(contains(json, "nan"));
    CHECK_FALSE(contains(json, "inf"));
    CHECK_FALSE(contains(json, "NaN"));
    CHECK_FALSE(contains(json, "Infinity"));
}

FRICTION_TEST(report, json_ignores_the_host_decimal_locale) {
    // A host that switches the global locale must not be able to turn the
    // decimal point into a comma and produce a document no parser accepts.
    Report report = buildSampleReport();
    report.process.elapsedMilliseconds = 1234.5;
    report.zones.front().selfMilliseconds = 12.5;

    const ScopedGlobalLocale locale(
        std::locale(std::locale::classic(), new CommaDecimalPunctuation));
    const std::string json = report.toJson(0);

    CHECK(contains(json, "\"elapsedMilliseconds\": 1234.5"));
    CHECK(contains(json, "\"selfMilliseconds\": 12.5"));
    CHECK_FALSE(contains(json, "1234,5"));
    CHECK_FALSE(contains(json, "12,5"));
}

FRICTION_TEST(report, an_empty_report_still_serializes) {
    const Report report;
    const std::string json = report.toJson();
    CHECK(contains(json, "\"zones\": ["));
    CHECK(contains(json, "\"counters\": ["));
    CHECK(report.empty());
    CHECK_NEAR(report.totalSelfMilliseconds(), 0.0, 1.0e-12);

    const std::string text = report.toString();
    CHECK(contains(text, "Friction report"));
    CHECK(contains(text, "unavailable on this platform"));
}

FRICTION_TEST(report, self_percentages_add_up) {
    const Report report = buildSampleReport();

    double total = 0.0;
    for (const auto& zone : report.zones) {
        CHECK(zone.selfPercent >= 0.0);
        CHECK(zone.selfPercent <= 100.0);
        total += zone.selfPercent;
    }
    CHECK_NEAR(total, 100.0, 0.01);
    CHECK_NEAR(
        report.totalSelfMilliseconds(),
        std::accumulate(report.zones.begin(), report.zones.end(), 0.0,
                        [](double sum, const auto& zone) { return sum + zone.selfMilliseconds; }),
        1.0e-9);
}

FRICTION_TEST(report, lookups_return_null_when_absent) {
    const Report report = buildSampleReport();
    CHECK(report.find("nothing here") == nullptr);
    CHECK(report.findCounter("nothing here") == nullptr);
    CHECK(report.find("outer") != nullptr);
}

FRICTION_TEST(report, process_section_summarizes_the_session) {
    Session session = Session::start();

    // The kernel accounts CPU time in whole units, so a session that returns
    // immediately can legitimately report zero. Burn enough of it that a
    // nonzero answer is the only possible one.
    const auto deadline = Clock::now() + Clock::frequency() / 100;
    volatile std::uint64_t burned = 0;
    while (Clock::now() < deadline) {
        for (int index = 0; index < 10'000; ++index) {
            burned += static_cast<std::uint64_t>(index);
        }
    }

    const Report report = session.stop();
    CHECK(burned > 0);

#if defined(FRICTION_OS_LINUX) || defined(FRICTION_OS_APPLE)
    CHECK(report.process.supported());
    CHECK(report.process.elapsedMilliseconds > 0.0);
    CHECK(report.process.userCpuMilliseconds > 0);
    CHECK(report.process.cpuUtilizationPercent > 0.0);
    CHECK(report.process.threadCount >= 1);
    CHECK(report.process.end.totalCpuNanoseconds() >= report.process.start.totalCpuNanoseconds());
#endif
    CHECK(contains(report.toJson(), "\"cpuUtilizationPercent\""));
}
