///
/// \file test_counters.cpp
///
/// Tests for the sharded counters, including that concurrent increments all land.
///
#include "harness.hpp"

#include <friction/friction.hpp>
#include <thread>
#include <vector>

using friction::Counter;
using friction::CounterSet;

FRICTION_TEST(counters, count_from_one_thread) {
    Counter counter;
    CHECK_EQ(counter.value(), 0ull);

    counter.add();
    counter.add(41);
    counter += 8;

    CHECK_EQ(counter.value(), 50ull);
    CHECK_EQ(counter.valueOnCallingThread(), 50ull);
}

FRICTION_TEST(counters, big_deltas_do_not_lose_precision) {
    Counter counter;
    counter.add(std::uint64_t{1} << 40);
    counter.add(1);
    CHECK_EQ(counter.value(), (std::uint64_t{1} << 40) + 1);
}

FRICTION_TEST(counters, reset_clears_every_shard) {
    Counter counter;
    counter.add(7);
    counter.reset();
    CHECK_EQ(counter.value(), 0ull);
    CHECK_EQ(counter.valueOnCallingThread(), 0ull);
}

FRICTION_TEST(counters, collect_every_threads_increments) {
    // The point of sharding: this has to be exact, and it has to stay exact when
    // the increments genuinely overlap.
    constexpr int kThreads = 8;
    constexpr std::uint64_t kPerThread = 200'000;

    Counter counter;
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int index = 0; index < kThreads; ++index) {
        workers.emplace_back([&counter, &start] {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (std::uint64_t iteration = 0; iteration < kPerThread; ++iteration) {
                counter.add();
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }

    CHECK_EQ(counter.value(), kPerThread * kThreads);
}

FRICTION_TEST(counters, spread_across_shards_under_contention) {
    // Each thread should land on its own shard, so the relaxed adds never
    // serialize on one word.
    constexpr int kThreads = 4;
    Counter counter;

    std::vector<std::uint64_t> perThread(kThreads, 0);
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int index = 0; index < kThreads; ++index) {
        workers.emplace_back([&counter, &perThread, index] {
            for (int iteration = 0; iteration < 1000; ++iteration) {
                counter.add();
            }
            perThread[static_cast<std::size_t>(index)] = counter.valueOnCallingThread();
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    std::uint64_t total = 0;
    for (const auto value : perThread) {
        total += value;
    }
    CHECK_EQ(counter.value(), total);
}

FRICTION_TEST(counter_set, returns_the_same_counter_for_the_same_name) {
    CounterSet counters;
    Counter& first = counters.counter("allocations");
    Counter& again = counters.counter("allocations");
    Counter& other = counters.counter("deallocations");

    CHECK_EQ(&first, &again);
    CHECK(&first != &other);
    CHECK_EQ(counters.size(), std::size_t{2});
    CHECK(counters.contains("allocations"));
    CHECK_FALSE(counters.contains("cache misses"));
}

FRICTION_TEST(counter_set, references_stay_valid_after_growth) {
    // The set is a vector of unique pointers precisely so that registering more
    // counters cannot move the ones a caller is already holding.
    CounterSet counters;
    Counter& first = counters.counter("first");
    first.add(11);

    for (int index = 0; index < 64; ++index) {
        (void)counters.counter("filler" + std::to_string(index));
    }

    CHECK_EQ(first.value(), 11ull);
    CHECK_EQ(counters.size(), std::size_t{65});
    CHECK_EQ(counters.name(0), std::string_view("first"));
    CHECK_EQ(counters.value(0), 11ull);
}

FRICTION_TEST(counter_set, reset_clears_everything) {
    CounterSet counters;
    counters.counter("a").add(3);
    counters.counter("b").add(4);
    counters.reset();
    CHECK_EQ(counters.value(0), 0ull);
    CHECK_EQ(counters.value(1), 0ull);
}

FRICTION_TEST(counter_set, empty_set_is_usable) {
    CounterSet counters;
    CHECK_EQ(counters.size(), std::size_t{0});
    CHECK_FALSE(counters.contains("nothing"));
    counters.reset();
    CHECK(true);
}
