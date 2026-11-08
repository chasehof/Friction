#pragma once

///
/// \file registry.hpp
///
/// The set of threads that have ever measured.
///
/// A thread links itself in once, on the first time it records anything, with a
/// compare-and-swap that never blocks. After that first touch the thread does
/// no atomic traffic at all while it measures.
///
/// Nodes are intentionally never freed. They hold the only copy of the data a
/// worker thread collected before it exited, so holding on to them is what lets
/// a report still show a thread pool's worth of work after every worker has
/// already joined. The cost is bounded by the number of threads that ever
/// measured -- hundreds of bytes each -- and it lasts exactly as long as the
/// process.
///
///
#include <friction/detail/platform.hpp>
#include <friction/detail/thread_state.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace friction::detail {

/// A node in the registry, reachable from the thread that created it.
///
/// The link is read while a session aggregates, so it is only ever published
/// once, by the owning thread, before that thread records anything.
struct RegisteredThread {
    RegisteredThread(std::size_t ordinal, const Histogram::Config& config) noexcept
        : state(config, ordinal) {}

    RegisteredThread* next = nullptr;
    ThreadState state;
};

/// Process-wide set of threads that have recorded a measurement.
class Registry final {
public:
    /// This thread's state, linking the thread in on first use.
    ///
    /// The pointer is held without a destructor so that exiting the thread does
    /// not take the node -- and the data in it -- back out of the registry.
    [[nodiscard]] static ThreadState& local(const Histogram::Config& config) noexcept {
        static thread_local RegisteredThread* node = nullptr;
        if (node == nullptr) {
            node = create(config);
        }
        return node->state;
    }

    /// Every registered thread, ordered by the sequence in which they first
    /// measured, so a report is reproducible run to run.
    [[nodiscard]] static std::vector<RegisteredThread*> snapshot() noexcept {
        std::vector<RegisteredThread*> nodes;

        for (auto* node = head().load(std::memory_order_acquire); node != nullptr;
             node = node->next) {
            nodes.push_back(node);
        }

        std::sort(nodes.begin(), nodes.end(),
                  [](const RegisteredThread* left, const RegisteredThread* right) {
                      return left->state.ordinal() < right->state.ordinal();
                  });
        return nodes;
    }

    [[nodiscard]] static std::size_t size() noexcept {
        std::size_t count = 0;
        for (auto* node = head().load(std::memory_order_acquire); node != nullptr; node = node->next) {
            ++count;
        }
        return count;
    }

    /// Frees every node and empties the registry.
    ///
    /// The registry deliberately outlives threads, so this exists for tests and
    /// for hosts that rebuild their world from scratch. Calling it while any
    /// thread is still measuring leaves dangling references.
    static void destroyAll() noexcept {
        auto* node = head().exchange(nullptr, std::memory_order_acq_rel);
        while (node != nullptr) {
            auto* next = node->next;
            delete node;
            node = next;
        }
    }

private:
    [[nodiscard]] static std::atomic<RegisteredThread*>& head() noexcept {
        static std::atomic<RegisteredThread*> registryHead{nullptr};
        return registryHead;
    }

    [[nodiscard]] static std::atomic<std::size_t>& nextOrdinal() noexcept {
        static std::atomic<std::size_t> ordinal{0};
        return ordinal;
    }

    [[nodiscard]] static RegisteredThread* create(const Histogram::Config& config) noexcept {
        const auto ordinal = nextOrdinal().fetch_add(1, std::memory_order_relaxed);
        auto* published = new RegisteredThread(ordinal, config);

        // Push onto the list. Another thread may have pushed between our load and
        // our store, in which case its list is the newer one and we retry.
        auto* observed = head().load(std::memory_order_relaxed);
        do {
            published->next = observed;
        } while (!head().compare_exchange_weak(observed, published, std::memory_order_release,
                                              std::memory_order_relaxed));

        return published;
    }

    Registry() = delete;
};

} // namespace friction::detail