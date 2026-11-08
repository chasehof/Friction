#pragma once

///
/// \file session_state.hpp
///
/// Whether anything is being measured right now.
///
/// One process-wide pointer, read on the hot path and written by whoever starts
/// or stops a session. A scope entered while it is null costs a single load and
/// a predictable branch, which is what lets the instrumentation stay in release
/// builds without being switched off.
///
///
#include <atomic>

#include <friction/histogram.hpp>

namespace friction::detail {

/// Configuration shared with every thread taking part in a session.
struct SessionState {
    Histogram::Config histogram{};
    std::uint64_t frequency = 0;

    /// Whether the `Session` that owns this state is still recording.
    ///
    /// The flag lives here rather than in the session because a session that
    /// opens another one has to be able to switch the first off without holding a
    /// pointer to it. Whoever opened last clears the flag on the state it
    /// displaced.
    bool recording = false;
};

/// Storage for the process-wide session pointer.
[[nodiscard]] inline std::atomic<SessionState*>& current() noexcept {
    static std::atomic<SessionState*> pointer{nullptr};
    return pointer;
}

/// The session currently recording, or null. Written only by start and stop.
///
/// Atomic because one thread publishes the pointer while every other thread
/// reads it to decide whether to record. Acquire and release are the pairing
/// that makes the session's configuration -- its histogram geometry above all --
/// visible to a thread that observes a non-null pointer, and on the architectures
/// this library targets they compile to the same plain load and store a
/// non-atomic pointer would have produced.
[[nodiscard]] inline SessionState* activeSession() noexcept {
    return current().load(std::memory_order_acquire);
}

/// Publishes the session, or clears it with nullptr.
inline void setActiveSession(SessionState* session) noexcept {
    current().store(session, std::memory_order_release);
}

} // namespace friction::detail
