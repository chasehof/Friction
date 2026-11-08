#pragma once

///
/// \file harness.hpp
///
/// A test harness small enough to read in one sitting.
///
/// Friction has no third party dependencies, and its tests do not add one
/// either: pulling a framework over the network to assert that `1 + 1 == 2` works
/// would make the build less reproducible than the thing it is testing.
///
///
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace friction::test {

/// Thrown by a failed check, caught by the runner.
struct AssertionFailure {
    std::string message;
};

/// True when the build has a sanitizer attached.
///
/// A sanitizer adds a shadow-memory access to every load and store, which makes
/// the whole program an order of magnitude slower. Any test that asserts on how
/// long something takes is measuring the sanitizer at that point, so those tests
/// stand down instead of reporting a number that was never about Friction.
inline constexpr bool sanitized() noexcept {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

/// Thrown by a test that has nothing to contribute in this build.
struct Skipped {
    std::string reason;
};

using TestBody = void (*)();

struct TestCase {
    std::string_view suite;
    std::string_view name;
    TestBody run;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

/// Registers a test at static initialization time.
struct Registrar {
    Registrar(std::string_view suite, std::string_view name, TestBody run) {
        registry().push_back(TestCase{suite, name, run});
    }
};

template <typename T>
[[nodiscard]] std::string describe(const T& value) {
    std::ostringstream out;
    if constexpr (std::is_same_v<T, bool>) {
        out << (value ? "true" : "false");
    } else {
        out << value;
    }
    return out.str();
}

[[noreturn]] inline void fail(const char* file, int line, const std::string& message) {
    throw AssertionFailure{std::string(file) + ":" + std::to_string(line) + ": " + message};
}

/// Runs every registered test, or only those whose name contains `filter`.
inline int runAll(std::string_view filter = {}) {
    std::size_t passed = 0;
    std::size_t skipped = 0;
    std::vector<std::string> failures;

    for (const auto& test : registry()) {
        const std::string full = std::string(test.suite) + "." + std::string(test.name);
        if (!filter.empty() && full.find(filter) == std::string::npos) {
            continue;
        }

        std::printf("[ run      ] %s\n", full.c_str());
        std::fflush(stdout);
        try {
            test.run();
            ++passed;
            std::printf("[       ok ] %s\n", full.c_str());
        } catch (const Skipped& skip) {
            ++skipped;
            std::printf("[  skipped ] %s\n    %s\n", full.c_str(), skip.reason.c_str());
        } catch (const AssertionFailure& failure) {
            failures.push_back(full + "\n    " + failure.message);
            std::printf("[  FAILED  ] %s\n    %s\n", full.c_str(), failure.message.c_str());
        } catch (const std::exception& error) {
            failures.push_back(full + "\n    unexpected exception: " + error.what());
            std::printf("[  FAILED  ] %s\n    unexpected exception: %s\n", full.c_str(),
                        error.what());
        }
        std::fflush(stdout);
    }

    std::printf("\n%zu passed, %zu failed", passed, failures.size());
    if (skipped != 0) {
        std::printf(", %zu skipped", skipped);
    }
    std::printf("\n");
    for (const auto& failure : failures) {
        std::printf("  %s\n", failure.c_str());
    }
    return failures.empty() ? 0 : 1;
}

} // namespace friction::test

#define FRICTION_TEST(suite, name)                                                                 \
    static void suite##_##name();                                                                  \
    static const ::friction::test::Registrar suite##_##name##_registrar{#suite, #name,             \
                                                                        &suite##_##name};          \
    static void suite##_##name()

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            ::friction::test::fail(__FILE__, __LINE__, "expected " #expression);                   \
        }                                                                                          \
    } while (false)

#define CHECK_FALSE(expression)                                                                    \
    do {                                                                                           \
        if ((expression)) {                                                                        \
            ::friction::test::fail(__FILE__, __LINE__, "expected not " #expression);               \
        }                                                                                          \
    } while (false)

#define CHECK_EQ(actual, expected)                                                                 \
    do {                                                                                           \
        const auto actualValue = (actual);                                                         \
        const auto expectedValue = (expected);                                                     \
        if (!(actualValue == expectedValue)) {                                                     \
            ::friction::test::fail(__FILE__, __LINE__,                                             \
                                   std::string(#actual) + " is " +                                 \
                                       ::friction::test::describe(actualValue) + ", expected " +   \
                                       ::friction::test::describe(expectedValue));                 \
        }                                                                                          \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                                    \
    do {                                                                                           \
        const double actualValue = static_cast<double>(actual);                                    \
        const double expectedValue = static_cast<double>(expected);                                \
        if (std::fabs(actualValue - expectedValue) > (tolerance)) {                                \
            ::friction::test::fail(                                                                \
                __FILE__, __LINE__,                                                                \
                std::string(#actual) + " is " + ::friction::test::describe(actualValue) +          \
                    ", expected " + ::friction::test::describe(expectedValue) + " within " +       \
                    ::friction::test::describe(static_cast<double>(tolerance)));                   \
        }                                                                                          \
    } while (false)

/// Stands the test down when the build makes its premise false.
#define SKIP_IF_SANITIZED(reason)                                                                  \
    do {                                                                                           \
        if (::friction::test::sanitized()) {                                                       \
            throw ::friction::test::Skipped{reason};                                               \
        }                                                                                          \
    } while (false)
