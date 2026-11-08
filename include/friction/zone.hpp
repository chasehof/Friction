#pragma once

///
/// \file zone.hpp
///
/// Timing scopes.
///
/// A scope is entered by the constructor and left by the destructor, so a
/// measurement cannot drift away from the code it wraps and cannot be left
/// unclosed by an early return or a thrown exception. Instrumenting a region is
/// a single line at the site you care about.
///
///     void parse(const std::string& text) {
///         FRICTION_ZONE("parse");
///         ...
///     }
///
/// While no session is recording, that same line costs one load of a global
/// pointer and a predictable branch. Nothing is timed and nothing is touched,
/// which is what makes it reasonable to leave instrumentation in release builds.
///
///
#include <friction/clock.hpp>
#include <friction/detail/platform.hpp>
#include <friction/detail/registry.hpp>
#include <friction/detail/session_state.hpp>
#include <friction/detail/thread_state.hpp>

#include <string_view>
#include <utility>

namespace friction {

/// Times the region it is declared in and records it on the way out.
///
/// Movable, so a scope can be held in a container or handed to a function that
/// decides when to finish it. A moved-from scope records nothing.
class ScopedZone final {
public:
    ScopedZone() noexcept = default;

    ScopedZone(std::string_view name) noexcept { enter(name); }

    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;

    ScopedZone(ScopedZone&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)), start_(std::exchange(other.start_, 0)) {}

    ScopedZone& operator=(ScopedZone&&) = delete;

    ~ScopedZone() noexcept { close(); }

    /// True while this scope is actually recording.
    [[nodiscard]] bool active() const noexcept { return state_ != nullptr; }

    /// Ends the scope early. Calling it twice, or letting the destructor run
    /// afterwards, is harmless.
    void close() noexcept {
        if (state_ != nullptr) {
            state_->leave(Clock::now() - start_);
            state_ = nullptr;
        }
    }

private:
    void enter(std::string_view name) noexcept {
        const detail::SessionState* const session = detail::activeSession();
        if (FRICTION_UNLIKELY(session == nullptr)) {
            return;
        }
        state_ = &detail::Registry::local(session->histogram);
        state_->enter(&state_->zone(name));
        start_ = Clock::now();
    }

    detail::ThreadState* state_ = nullptr;
    Clock::Ticks start_ = 0;
};

} // namespace friction

#define FRICTION_DETAIL_ZONE_VARIABLE_2(line) friction_zone_##line
#define FRICTION_DETAIL_ZONE_VARIABLE(line) FRICTION_DETAIL_ZONE_VARIABLE_2(line)

/// Records the enclosing scope under a name.
///
/// The name should be a string literal. Friction keys regions on the address of
/// the literal, so passing one is what keeps the lookup a hash probe instead of
/// a string comparison.
///
/// The macro declares a variable, so the measurement covers the rest of the
/// enclosing block, not the rest of the statement. At the top of a function,
/// which is where instrumentation belongs, the two are the same thing:
///
///     void parseFrame(const Buffer& buffer) {
///         FRICTION_ZONE("parseFrame");
///         ...
///     }
///
/// Inside a loop or any other block you do not want to measure, wrap it:
///
///     for (const auto& request : queue) {
///         { FRICTION_ZONE("handleRequest"); handle(request); }
///     }
///
/// Getting this wrong is silent in one direction only: a zone left open when the
/// session stops loses its sample, and a zone closed early measures less than it
/// wrapped. Neither shows up as an error.
#define FRICTION_ZONE(name) ::friction::ScopedZone FRICTION_DETAIL_ZONE_VARIABLE(__LINE__) { name }