#pragma once
#include <memory>
#include <cstdint>

struct Snapshot {
    uint64_t l1_cache_misses {0};
    uint64_t l2_cache_misses {0};
    uint64_t allocations {0};
    uint64_t deletions {0};
    uint64_t involuntary_context_switches {0};
    uint64_t voluntary_context_switches {0};
    uint64_t syscalls {0};
};

class Friction {
public: 
    Friction();
    ~Friction();

    void start(); 
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};
