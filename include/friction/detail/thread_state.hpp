#pragma once

///
/// \file thread_state.hpp
///
/// The landing zone for everything one thread measures.
///
/// Samples are recorded into state owned by the thread that took them, so the
/// common case touches no atomics and takes no locks. Merging happens once, at
/// the end, off the hot path.
///
/// Zones are found by the address of the name literal at the instrumentation
/// site rather than by comparing name text. A call site passes the same address
/// every time it runs, so the lookup is a hash and a probe instead of a string
/// comparison, and a hot loop alternating between several instrumented regions
/// still costs the same as one that repeats a single region.
///
///
#include <friction/clock.hpp>
#include <friction/histogram.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace friction::detail {

/// One named region's accumulated samples for one thread.
struct ZoneSlot {
    ZoneSlot(std::string zoneName, const Histogram::Config& config)
        : name(std::move(zoneName)), inclusive(config), selfTime(config) {
    }

    /// Name exactly as written at the instrumentation site.
    std::string name;

    /// Time in the region, children included.
    Histogram inclusive;

    /// Time in the region with children subtracted.
    ///
    /// A region no child ever entered has identical inclusive and self time, but
    /// the second histogram is one allocation the library makes per region per
    /// thread up front rather than a branch and a potential allocation on the
    /// way out of every nested scope.
    Histogram selfTime;

    /// Adopts a new geometry and drops the samples recorded against the old one.
    void configure(const Histogram::Config& config) noexcept {
        inclusive.configure(config);
        selfTime.configure(config);
    }

    [[nodiscard]] std::string_view nameView() const noexcept {
        return name;
    }
};

/// Everything one thread contributes to a session.
class ThreadState final {
    public:
    /// Instrumented regions nested deeper than this are measured but stop
    /// charging their time to a parent, so their time is not lost.
    static constexpr std::size_t kMaxNestingDepth = 64;

    ThreadState(Histogram::Config config, std::size_t ordinal) noexcept
        : config_(config), ordinal_(ordinal) {
    }

    ThreadState(const ThreadState&) = delete;
    ThreadState& operator=(const ThreadState&) = delete;

    /// Returns the slot for `name`, creating it the first time this thread
    /// measures that region. Valid until `reset()`.
    ///
    /// The common case is one hash and one pointer comparison: the table maps the
    /// address of the name literal to the slot created for it. A literal with the
    /// same spelling at a second call site is a different address, so a miss falls
    /// back to comparing the text once and then teaches the table about the new
    /// address.
    ZoneSlot& zone(std::string_view name) {
        const char* const key = name.data();
        std::size_t probe = probeSlotFor(key);
        std::size_t freeSlot = kZoneTableSize;

        for (std::size_t step = 0; step < kZoneTableSize; ++step) {
            const std::uint32_t entry = lookupTable_[probe];
            if (entry == kEmptySlot) {
                freeSlot = probe;
                break;
            }
            if (keys_[probe] == key) {
                return *zones_[entry - 1];
            }
            probe = (probe + 1) & (kZoneTableSize - 1);
        }

        for (std::size_t index = 0; index < zones_.size(); ++index) {
            if (zones_[index]->nameView() == name) {
                remember(freeSlot, key, index);
                return *zones_[index];
            }
        }

        zones_.push_back(std::make_unique<ZoneSlot>(std::string(name), config_));
        remember(freeSlot, key, zones_.size() - 1);
        return *zones_.back();
    }

    void enter(ZoneSlot* slot) noexcept {
        if (depth_ < kMaxNestingDepth) {
            stack_[depth_] = Frame{slot, 0};
        }
        ++depth_;
    }

    /// Books the elapsed time of the innermost region.
    ///
    /// The region's inclusive time is what it measured. Its self time is what is
    /// left after subtracting the time its children spent inside it, which is
    /// only knowable here, as each child closes, by accumulating into the frame
    /// that opened before it.
    void leave(Clock::Ticks elapsed) noexcept {
        if (depth_ == 0) {
            return;
        }
        const std::size_t index = std::min(depth_, kMaxNestingDepth) - 1;
        Frame& frame = stack_[index];

        frame.slot->inclusive.record(elapsed);
        frame.slot->selfTime.record(elapsed > frame.childTicks ? elapsed - frame.childTicks : 0);

        if (index > 0) {
            // Hand the time to the enclosing region, so its self time comes out
            // already reduced by everything it contained.
            stack_[index - 1].childTicks += elapsed;
        }
        --depth_;
    }

    /// Drops every recorded sample and adopts the session's geometry, as when a
    /// new session begins.
    ///
    /// The slots themselves are kept rather than freed, so a scope that was open
    /// across the boundary still has something valid to write to. A session is
    /// expected to start with the previous work quiesced, but a thread that is
    /// unwinding through an instrumented frame should not be left holding a
    /// pointer into freed memory.
    void reset(const Histogram::Config& config) noexcept {
        config_ = config;
        for (auto& slot : zones_) {
            slot->configure(config);
        }
        depth_ = 0;
        lookupTable_.fill(kEmptySlot);
        keys_.fill(nullptr);
    }

    [[nodiscard]] const std::vector<std::unique_ptr<ZoneSlot>>& zones() const noexcept {
        return zones_;
    }
    [[nodiscard]] const Histogram::Config& histogramConfig() const noexcept {
        return config_;
    }
    [[nodiscard]] std::size_t ordinal() const noexcept {
        return ordinal_;
    }
    [[nodiscard]] std::size_t zoneCount() const noexcept {
        return zones_.size();
    }

    private:
    static constexpr std::size_t kZoneTableSize = 128; // power of two
    static constexpr std::uint32_t kEmptySlot = 0;

    /// One open scope, and the running total of what its children have spent.
    struct Frame {
        ZoneSlot* slot = nullptr;
        Clock::Ticks childTicks = 0;
    };

    /// Scatters string-literal addresses across the table. Finalizer mix, so a
    /// handful of call sites does not pile into one probe run.
    [[nodiscard]] static std::size_t probeSlotFor(const char* name) noexcept {
        auto key = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(name));
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdull;
        key ^= key >> 33;
        return static_cast<std::size_t>(key) & (kZoneTableSize - 1);
    }

    /// Points the table's free probe slot at an existing zone, or does nothing
    /// when the table is full. `freeSlot == kZoneTableSize` is the full case.
    void remember(std::size_t freeSlot, const char* key, std::size_t index) noexcept {
        if (freeSlot < kZoneTableSize) {
            lookupTable_[freeSlot] = static_cast<std::uint32_t>(index + 1);
            keys_[freeSlot] = key;
        }
    }

    Histogram::Config config_;
    std::size_t ordinal_ = 0;
    std::size_t depth_ = 0;
    std::vector<std::unique_ptr<ZoneSlot>> zones_;
    std::array<Frame, kMaxNestingDepth> stack_{};
    std::array<std::uint32_t, kZoneTableSize> lookupTable_{};
    std::array<const char*, kZoneTableSize> keys_{};
};

} // namespace friction::detail
