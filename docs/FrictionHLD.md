# Friction High Level Design

This document describes the architecture of Friction as built: the components, the boundaries between them, and the reasoning that produced them. It is meant to be read alongside the headers, which carry the local reasoning for their own code.

## What Friction is

A measurement library that stays inside the process being measured. It answers one question — where did the time go — for regions the developer chose to mark, plus whatever the operating system will say about the process for free.

It is not a telemetry pipeline. There is no background thread, no queue between producers and consumers, and no dashboard. The earlier design in this repository proposed exactly that, and it was wrong for this problem: a queue implies a producer that has already paid to measure, and a consumer that has to be scheduled, drained, and defended against re-entrancy. All of that cost is charged to the program being measured in exchange for convenience nobody asked for. A measurement tool that slows the thing it measures enough to change the answer is measuring something else.

## Design constraints

These constraints are ordered. When they conflict, the earlier one wins.

1. **Accuracy before convenience.** The instrumentation must not change the program enough to change the result. Every design decision below is downstream of this.
2. **No waiting.** No mutex, no condition variable, no blocking allocation on a path that runs while the program is working. A measurement that can block is a measurement that measures the blocking.
3. **No thread of its own.** Friction never starts a thread. Anything it did needed that thread to be scheduled, which is a latency the measured program pays for and cannot see.
4. **Bounded everything.** Memory per region, per thread, and per counter is fixed at setup and does not grow with the number of samples. A long-running service must not slowly drown in its own instrumentation.
5. **Leave it in release builds.** Instrumentation that must be compiled out to be affordable is instrumentation nobody uses. A zone in a build with no session open costs a load and a branch.

## System overview

Four components, and the boundaries between them are the design.

```
   instrumented code
          |
          v
   ScopedZone            zone.hpp
          |
          |  one global pointer test
          v
   SessionState          detail/session_state.hpp
          |
          |  per-thread, no shared mutable state
          v
   ThreadState           detail/thread_state.hpp   <-- Histogram (histogram.hpp)
          |
          |  merge once, at stop()
          v
   Session -> Report     session.hpp, report.hpp   <-- ProcessStats (process.hpp)
```

### `ScopedZone` — the hot path

Entering and leaving a scope does four things: read a global pointer, find the region, read the clock, and record the sample. That is the entire cost model.

The global pointer test is what satisfies constraint 5. A scope entered while nothing is recording does no work at all — the load and the branch are the whole operation, measured at 0.4 ns.

Regions are found by the address of the name literal, not by comparing text. A call site passes the same address every time it runs, so the lookup is a hash and one probe. The cost is independent of how many distinct regions a hot path alternates between, and no string is ever touched.

### `SessionState` — the one piece of shared mutable state

A single process-wide pointer, read on the hot path and written only by whoever starts or stops a session. Everything else about a session is per-thread.

This is where the "one session at a time" constraint comes from, and it is a deliberate trade rather than a limitation we failed to design around. A scope has to decide whether it is recording, and it has to decide that with something that is cheap to read and safe to race on. One pointer is the answer. Supporting concurrent sessions would mean a list, a generation counter, and a per-thread "which session am I in" lookup on every entry — paid by every scope, to serve a case (two independent measurements of the same process at once) that is rare enough to solve by running them in sequence.

The recording flag lives in the state rather than in the `Session` object so that opening a new session can switch off the one it displaces without holding a pointer to it. A superseded session reports empty rather than reporting work that belongs to its successor, and stopping it does not switch off the session that replaced it.

### `ThreadState` — where samples live until the session ends

Each thread that records anything owns the storage its samples go into. This is what satisfies constraints 2 and 4 with no coordination: a thread increments a counter in its own memory, and no two threads write the same bytes.

Nested regions are handled with a small stack of frames. Each frame remembers the region and the running total of what its children have spent. When a scope closes, its elapsed time is recorded as inclusive time, and as self time once its children's time is subtracted. The subtraction happens on the way out rather than at merge time because that is the only moment the answer is known.

Frames are capped at 64. Regions nested deeper are measured but stop charging their time to a parent, so the time is still reported rather than lost.

`reset()` clears samples and adopts the session's histogram geometry but keeps the slots allocated, so a thread unwinding through an instrumented frame across a session boundary still holds a valid pointer.

### `Registry` — how threads are found at the end

A thread links itself in once, on the first time it records, with a compare-and-swap that never blocks. After that first touch it does no atomic traffic at all while it measures. The list is walked exactly once, at `stop()`.

Nodes are never freed. They hold the only copy of the data a worker thread collected before it exited, so keeping them is what lets a report still show a thread pool's worth of work after every worker has joined. The cost is bounded by the number of threads that ever measured and lasts as long as the process.

### `Histogram` — bounded memory, honest percentiles

A log-linear layout: a fixed number of slots per power of ten, so relative accuracy is constant across the whole range instead of degrading toward the top. Two histograms per region per thread — inclusive and self — are allocated up front rather than one being computed from the other on the way out, because that trade would put an allocation on the exit path of every nested scope.

Samples are reported as the top of the bucket they landed in, so a percentile never understates the sample it stands for, and then clamped to the range actually observed. A bucket's top edge can sit above the largest sample recorded — a sample of 200 ticks lands in a bucket reaching 204 — and a report claiming a 99th percentile larger than its own maximum is not a more accurate answer, it is a wrong one.

The standard deviation is derived from the slots rather than from a running sum of squares, which loses precision once samples span a wide range of magnitudes.

Anything past the configured range saturates into the boundary slot, and the report says so, because a saturated percentile is a floor and not a measurement.

### `Session` and `Report` — off the hot path entirely

Everything expensive happens here, once, at `stop()`: merge every thread's samples, sort regions by self time, convert ticks to nanoseconds, sample the OS counters, and format.

No hot path ever sees a `double`. Timestamps are integer ticks end to end and are converted exactly once, at report time. That is what keeps a 2.5 GHz counter's resolution intact instead of losing precision to repeated floating-point division.

`Report` holds plain values. It has no back-pointer to the session, so it outlives whatever produced it and can be stored, serialized, or handed to something else entirely.

### `ProcessStats` — what the OS will give away for free

Linux reads `/proc/self/status` and `/proc/self/stat`; macOS uses `getrusage`. These are the counters that cost nothing to obtain: RSS, peak RSS, page faults, context switches, CPU time, thread count.

Sampling happens twice per session — at start and at stop — and nowhere else. Each sample reads a couple of files, which is nothing next to a workload but is not something to pay twice a second in a hot loop. `SessionConfig::sampleProcess` turns it off when the workload is small enough for the difference to matter.

On platforms with no source for these counters the report says `unsupported` rather than reporting zeros, because a zero that means "we did not look" is worse than an admission.

## Data flow

The whole data flow, in order:

1. A thread enters a scope. The global pointer says whether anything is recording.
2. If so, the thread finds or creates its slot for that region and reads the clock.
3. On exit, it records inclusive and self time into its own histograms and hands its elapsed time to the enclosing frame.
4. At `stop()`, every registered thread's histograms are merged by region name into one distribution per region, and the OS counters are read.
5. `Report` is built and returned. It is a value; nothing further happens.

There is no queue, no consumer, and no intermediate representation. A sample is written once, in the form it will be reported in, and read once.

## Deliberately not built

- **A ring buffer between producers and consumers.** The data pathway in the previous design was its centerpiece and is absent here. Producers and consumers are the same thread, and the queue it described had nothing to carry.
- **PMU counters.** Cache misses and branch misses need hardware performance counters, which are privileged on Linux and unavailable to an unprivileged process. Friction reports what can be had unprivileged and leaves the rest to a sampling profiler that can afford to interrupt the program.
- **Memory allocation and syscall tracing.** Both need to intercept the thing being measured, which means interposition, platform-specific machinery, and a distortion the measurement is supposed to avoid.
- **A display.** `toString()` writes to a terminal, `toJson()` writes to a pipe. Deciding what to do with the output is the host program's job, not the library's.

## Build and distribution

The library is header-only and split into modular headers under `include/friction/`. `tools/amalgamate.py` inlines them, in dependency order, into `single_include/friction/friction.hpp`, which is checked in.

Only includes outside every conditional block are hoisted to the top of the generated header. Anything inside one — `<fcntl.h>` behind an `#if` for Linux, say — stays where it was, because lifting it would ask a Windows compiler for a header that does not exist there.

The check runs as a test. A generated header that has drifted from its sources fails the build, which is the only way the promise that the single header works can be kept honest.

## Testing

The suite has no external dependency. `tests/harness.hpp` is a small registry-based runner, so a clone builds and tests with nothing but a compiler and CMake.

The suite builds twice, once per include path, and the overhead tests assert ceilings rather than printing numbers for a human to judge. The ceilings are loose enough to survive a loaded build machine, and tight enough that a mutex, an allocation, or a syscall appearing on the measurement path fails the build.
