# Friction

Scoped timing, counters, and OS process metrics for C++20, with instrumentation cheap enough to leave in release builds.

```cpp
#include <friction/friction.hpp>

void parseFrame(const Buffer& buffer) {
    FRICTION_ZONE("parseFrame");
    ...
}

int main() {
    auto session = friction::Session::start();
    serveRequests();
    const friction::Report report = session.stop();
    std::puts(report.toString().c_str());
}
```

```
Friction report
  session    0.001s wall, 1 thread, clock TSC at 2.496 GHz
  process    rss 3.32 MiB (+8.00 KiB over the session), peak 3.32 MiB
  faults     2 minor, 0 major
  switches   0 voluntary, 0 involuntary, cpu 83.3% of 0.001s

  region                          self%        self      total    calls       mean         p50         p99         max
  parseFrame                      95.6%       0.001s       0.001s     1000     635.37ns     615.00ns     974.00ns      15.38us
  decode                          4.4%       0.000s       0.000s     1000      27.68ns      26.00ns      46.00ns     242.00ns
```

One header, no linking, no background threads, no lock-free ring buffer to drain. A region is measured by two timestamp reads and a counter bump; nothing else happens while your program runs.

## What it gives you

- **Timing distributions, not averages.** Every sample goes into a bounded log-linear histogram, so p99 and max are real measurements rather than the mean wearing a hat. Range and resolution are yours to choose.
- **Inclusive and self time.** Nesting is subtracted, so the report answers "where did the time actually go" rather than "what called what".
- **Counters.** Named, cache-line-sharded, relaxed-atomic. Safe to increment from any thread with no coordination.
- **Real OS counters.** RSS, peak RSS, page faults, context switches, CPU time, thread count, sampled at session start and stop.
- **Text or JSON.** `Report::toString()` for a terminal, `Report::toJson()` for whatever you pipe it into.

## Installing

### Single header

`single_include/friction/friction.hpp` is generated from the modular headers and is checked in. Copy it, or point your include path at `single_include/`, and include it:

```cpp
#include <friction/friction.hpp>
```

### CMake

```cmake
find_package(Friction REQUIRED)
target_link_libraries(your_target PRIVATE Friction::Friction)
```

Or vendor the repository and `add_subdirectory` it. Either way there is nothing to link; the target carries include paths and a C++20 requirement.

### Building and testing

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The suite builds twice — once against the modular headers, once against the generated single header — so an amalgamation that stops matching the sources fails the build rather than surprising a consumer.

To regenerate the single header after editing anything under `include/`:

```bash
cmake --build build --target friction_amalgamate
```

## Using it

`FRICTION_ZONE` should be a string literal. Friction keys regions on the address of the literal, which is what keeps the lookup a hash probe instead of a string comparison.

**The macro declares a variable, so a zone covers the rest of the enclosing block, not the rest of the statement.** At the top of a function, where instrumentation belongs, those are the same thing:

```cpp
void parseFrame(const Buffer& buffer) {
    FRICTION_ZONE("parseFrame");   // covers the rest of parseFrame
    ...
}
```

Inside a loop or any other block you do not want to measure, wrap it:

```cpp
for (const auto& request : queue) {
    { FRICTION_ZONE("handleRequest"); handle(request); }
}
```

Getting this wrong fails silently in one direction only: a zone left open when the session stops loses its sample, and a zone closed early measures less than it wrapped.

While no session is recording, that same line costs a load of one global pointer and a predictable branch, so instrumentation can stay compiled into release builds:

```
scope with no session          0.4 ns
```

### Counters

Register names during setup and keep the reference; resolving a name allocates.

```cpp
auto& bytes = session.counters().counter("response.bytes");
bytes.add(payload.size());
```

```
counter increment              9.1 ns
```

### Configuration

```cpp
friction::SessionConfig config;
config.histogram.highestTrackableValue = 1 << 20;  // samples saturate above this
config.histogram.significantFigures = 2;           // ~0.8% relative error
config.sampleProcess = false;                      // skip the /proc reads

auto session = friction::Session::start(config);
```

## What it costs

Measured by `tests/test_overhead.cpp`, which asserts ceilings rather than printing them. On a 2.5 GHz x86 core:

| | |
|---|---|
| scope with no session open | 0.4 ns |
| scope enter and leave, recording | ~55 ns |
| nested inner scope | ~55 ns |
| counter increment | ~9 ns |
| counter name resolution, once | ~5 ns |
| histogram memory, fixed | 1984 bytes |

A scope added to a ~300 ns body costs about 40–50 ns, or 13–17%. That is the honest number, and it is dominated by the two timestamp reads. It is the price of measuring rather than the price of the mechanism: the ceilings in the test suite exist to catch a lock, an allocation, or a syscall appearing on that path.

Memory is bounded and does not grow with samples. Threads that have exited keep their collected samples, so a thread pool's work still appears in the report after every worker has joined.

## Limits worth knowing

- **One session per process, at a time.** Starting a second session stops the first, whose report is then lost. This is what lets a scope check a single pointer to decide whether it is measuring anything.
- **Start and stop from one thread, with the work quiesced.** A session left running keeps instrumented code on its recording path.
- **Process counters are Linux and macOS.** Linux reads `/proc/self/status` and `/proc/self/stat`; macOS uses `getrusage`. Elsewhere the report says so rather than reporting zeros. Windows process metrics are not implemented.
- **No PMU counters.** Cache misses, branch misses, and the rest need performance counters, which are privileged on Linux. Friction reports what the OS will hand out unprivileged and leaves the rest to a sampling profiler.
- **Names are keyed on the literal's address.** Two `FRICTION_ZONE("work")` lines in different places are two different regions. That is deliberate — a shared key would need a string comparison — but it means regions do not merge across call sites.

## Design notes

`docs/FrictionHLD.md` covers the architecture and the reasoning behind it. The short version: one global pointer decides whether anything is being measured, per-thread state keeps samples off shared state, and all merging happens once at `stop()`.

## License

MIT. See [LICENSE](LICENSE).