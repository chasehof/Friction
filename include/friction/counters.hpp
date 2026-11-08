#pragma once

///
/// \file counters.hpp
///
/// Counters for things a timer cannot see: allocations, cache misses, retries,
/// bytes on the wire.
///
/// Every counter is split into cache-line-padded shards and the calling thread
/// only ever touches its own. That turns a single `lock xadd` on one shared
/// word -- which serializes as soon as two cores are incrementing -- into an
/// uncontended store to a private line. Reading sums the shards, which is the
/// only step that has to think about concurrency at all.
///
///
#include <friction/detail/platform.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace friction {

/// A counter safe to increment from any thread without contention.
class Counter final {
public:
    /// Number of shards. Must be a power of two.
    static constexpr std::size_t kShardCount = 8;

    static_assert((kShardCount & (kShardCount - 1)) == 0, "kShardCount must be a power of two");

    /// Increments by `delta`, or by one if none is given.
    void add(std::uint64_t delta = 1) noexcept {
        shard().value.fetch_add(delta, std::memory_order_relaxed);
    }

    void operator+=(std::uint64_t delta) noexcept { add(delta); }

    /// Sum across every shard. Costs one pass over the shards, so it belongs off
    /// the hot path.
    [[nodiscard]] std::uint64_t value() const noexcept {
        std::uint64_t total = 0;
        for (const auto& shard : shards_) {
            total += shard.value.load(std::memory_order_relaxed);
        }
        return total;
    }

    /// This thread's contribution alone.
    [[nodiscard]] std::uint64_t valueOnCallingThread() const noexcept {
        return shard().value.load(std::memory_order_relaxed);
    }

    /// Zeroes every shard. Only meaningful once the owning threads have quiesced.
    void reset() noexcept {
        for (auto& shard : shards_) {
            shard.value.store(0, std::memory_order_relaxed);
        }
    }

private:
    /// One counter line. Padding keeps two counters from landing on the same one.
    struct alignas(detail::kCacheLineSize) Shard {
        std::atomic<std::uint64_t> value{0};
    };

    [[nodiscard]] static std::size_t shardIndex() noexcept {
        // Threads are numbered in the order they first touch Friction and the
        // numbering is stable for the life of the thread, so a thread keeps
        // hitting the same shard. Threads past the shard count wrap around and
        // share; correctness is unaffected, only the contention profile is.
        static std::atomic<std::size_t> nextThreadOrdinal{0};
        static thread_local std::size_t ordinal = nextThreadOrdinal.fetch_add(1, std::memory_order_relaxed);
        return ordinal & (kShardCount - 1);
    }

    [[nodiscard]] const Shard& shard() const noexcept { return shards_[shardIndex()]; }

    [[nodiscard]] Shard& shard() noexcept { return shards_[shardIndex()]; }

    Shard shards_[kShardCount];
};

/// A named collection of counters.
///
/// Names are resolved when a counter is first asked for and the returned
/// reference stays valid for the life of the set, so callers hoist the reference
/// out and only ever pay the lookup once during setup.
///
/// Registering a new name appends to the set and is therefore not thread-safe;
/// do it before starting the threads that will increment.
class CounterSet final {
public:
    /// Returns the counter called `name`, creating it if this set has not seen it.
    [[nodiscard]] Counter& counter(std::string_view name) {
        for (std::size_t index = 0; index < names_.size(); ++index) {
            if (names_[index] == name) {
                return *counters_[index];
            }
        }
        names_.emplace_back(name);
        counters_.push_back(std::make_unique<Counter>());
        return *counters_.back();
    }

    /// Name of the counter at `index`, in registration order.
    [[nodiscard]] std::string_view name(std::size_t index) const noexcept { return names_[index]; }

    [[nodiscard]] std::size_t size() const noexcept { return counters_.size(); }

    [[nodiscard]] bool contains(std::string_view name) const noexcept {
        for (const auto& candidate : names_) {
            if (candidate == name) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::uint64_t value(std::size_t index) const noexcept { return counters_[index]->value(); }

    [[nodiscard]] Counter& operator[](std::size_t index) noexcept { return *counters_[index]; }

    /// Zeroes every counter.
    void reset() noexcept {
        for (auto& counter : counters_) {
            counter->reset();
        }
    }

private:
    // unique_ptr keeps each Counter's address fixed as the vector reallocates.
    std::vector<std::string> names_;
    std::vector<std::unique_ptr<Counter>> counters_;
};

} // namespace friction