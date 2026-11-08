#pragma once

///
/// \file process.hpp
///
/// Numbers the operating system knows about this process.
///
/// A timer can only see the region you wrapped. These are the things a timing
/// scope cannot: how much memory the process is holding, how often it is being
/// scheduled out, how much work the kernel is doing on its behalf, and how much
/// CPU it is actually burning. Together they are what tells you whether a slow
/// function is slow because of its own instructions or because of the allocator,
/// the page faults, or the contention around it.
///
/// Everything here is a read-only sample of kernel counters. Reading them costs
/// the profiled process a couple of syscalls, so they are sampled when a session
/// starts and stops rather than continuously.
///
///
#include <friction/detail/platform.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

#if defined(FRICTION_OS_LINUX)
#include <cstring>

#include <fcntl.h>
#include <unistd.h>
#endif

#if defined(FRICTION_OS_LINUX) || defined(FRICTION_OS_APPLE)
#include <sys/resource.h>
#include <sys/time.h>
#endif

namespace friction {

/// Where a set of process numbers came from.
enum class ProcessSource {
    unknown, ///< Nothing is available on this platform.
    procfs,  ///< Linux, reading /proc/self.
    rusage,  ///< CPU time and faults from getrusage, nothing else.
};

/// A sample of this process's resource counters.
struct ProcessStats {
    ProcessSource source = ProcessSource::unknown;

    /// Resident set size, and the high water mark it reached since start.
    std::uint64_t residentBytes = 0;
    std::uint64_t peakResidentBytes = 0;

    /// Address space reserved, which includes mappings not backed by memory.
    std::uint64_t virtualBytes = 0;

    /// Memory the kernel had to fetch from disk.
    std::uint64_t majorFaults = 0;

    /// Memory the kernel satisfied from around the process.
    std::uint64_t minorFaults = 0;

    /// Times the process gave up the CPU of its own accord, usually on a read.
    std::uint64_t voluntaryContextSwitches = 0;

    /// Times the process was preempted. A climbing count under a flat CPU
    /// profile means the thread is starved rather than slow.
    std::uint64_t involuntaryContextSwitches = 0;

    /// CPU time consumed in user mode and in the kernel.
    std::uint64_t userCpuNanoseconds = 0;
    std::uint64_t systemCpuNanoseconds = 0;

    /// Threads the kernel currently has for this process.
    std::uint32_t threadCount = 0;

    /// True when resident memory was reported.
    [[nodiscard]] bool hasResidentMemory() const noexcept {
        return source == ProcessSource::procfs && residentBytes != 0;
    }

    /// True when scheduling counters were reported.
    [[nodiscard]] bool hasContextSwitches() const noexcept {
        return source == ProcessSource::procfs &&
               (voluntaryContextSwitches != 0 || involuntaryContextSwitches != 0);
    }

    /// CPU time from both modes.
    [[nodiscard]] std::uint64_t totalCpuNanoseconds() const noexcept {
        return userCpuNanoseconds + systemCpuNanoseconds;
    }
};

/// Reads the current process's resource counters.
///
/// Never throws and never allocates: on failure it returns an all-zero sample
/// whose `source` says so, rather than pretending the process is idle.
[[nodiscard]] ProcessStats readProcessStats() noexcept;

/// True when this build can read real process counters from the platform.
[[nodiscard]] bool processStatsSupported() noexcept;

namespace detail {

#if defined(FRICTION_OS_LINUX)

/// A `/proc` file read into a fixed buffer, so sampling cannot allocate.
class ProcFile final {
    public:
    ProcFile() noexcept = default;

    ~ProcFile() noexcept;

    ProcFile(const ProcFile&) = delete;
    ProcFile& operator=(const ProcFile&) = delete;

    /// Reads `path`, truncating rather than growing if it is unusually long.
    [[nodiscard]] bool open(const char* path) noexcept;

    /// Finds a line starting with `key` and parses the number on it.
    [[nodiscard]] bool findUnsigned(std::string_view key, std::uint64_t& out) const noexcept;

    /// Finds a line starting with `key` and parses its value with its unit,
    /// scaling the kB, MB and GB suffixes `/proc` uses to bytes.
    [[nodiscard]] bool findByteCount(std::string_view key, std::uint64_t& out) const noexcept;

    /// Parses a field of `/proc/self/stat` by its number in proc(5), where
    /// fields 1 and 2 are the pid and the parenthesised command name.
    [[nodiscard]] bool readStatField(std::size_t field, std::uint64_t& out) const noexcept;

    private:
    static constexpr std::size_t kCapacity = 8192;

    [[nodiscard]] const char* end() const noexcept {
        return buffer_ + size_;
    }

    /// Locates the start of the value following `key`, or null if the key is
    /// absent. Only matches at the start of a line, so `RssAnon` cannot be
    /// mistaken for `VmRSS`.
    [[nodiscard]] const char* findValueFor(std::string_view key) const noexcept;

    [[nodiscard]] static const char* skipSpaces(const char* cursor, const char* end) noexcept;
    /// Parses a decimal integer at `cursor` and leaves it on the first character
    /// that is not part of the number, so the caller can read a unit suffix.
    [[nodiscard]] static bool parseUnsigned(const char*& cursor, const char* end,
                                            std::uint64_t& out) noexcept;

    char buffer_[kCapacity]{};
    std::size_t size_ = 0;
    int descriptor_ = -1;
};

#endif // FRICTION_OS_LINUX

} // namespace detail

#if defined(FRICTION_OS_LINUX)

namespace detail {

inline ProcFile::~ProcFile() noexcept {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
    }
}

inline bool ProcFile::open(const char* path) noexcept {
    descriptor_ = ::open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor_ < 0) {
        return false;
    }
    size_ = 0;
    while (size_ < kCapacity) {
        const auto read = ::read(descriptor_, buffer_ + size_, kCapacity - size_);
        if (read <= 0) {
            break;
        }
        size_ += static_cast<std::size_t>(read);
    }
    return size_ > 0;
}

inline const char* ProcFile::skipSpaces(const char* cursor, const char* end) noexcept {
    while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
        ++cursor;
    }
    return cursor;
}

inline bool ProcFile::parseUnsigned(const char*& cursor, const char* end,
                                    std::uint64_t& out) noexcept {
    cursor = skipSpaces(cursor, end);
    if (cursor == end || *cursor < '0' || *cursor > '9') {
        return false;
    }
    std::uint64_t value = 0;
    while (cursor < end && *cursor >= '0' && *cursor <= '9') {
        value = value * 10 + static_cast<std::uint64_t>(*cursor - '0');
        ++cursor;
    }
    out = value;
    return true;
}

inline const char* ProcFile::findValueFor(std::string_view key) const noexcept {
    std::size_t offset = 0;
    while (offset < size_) {
        if (std::memcmp(buffer_ + offset, key.data(), key.size()) == 0 &&
            (offset == 0 || buffer_[offset - 1] == '\n')) {
            return buffer_ + offset + key.size();
        }
        const auto* lineBreak =
            static_cast<const char*>(std::memchr(buffer_ + offset, '\n', size_ - offset));
        if (lineBreak == nullptr) {
            return nullptr;
        }
        offset = static_cast<std::size_t>(lineBreak - buffer_) + 1;
    }
    return nullptr;
}

inline bool ProcFile::findUnsigned(std::string_view key, std::uint64_t& out) const noexcept {
    const char* cursor = findValueFor(key);
    return cursor != nullptr && parseUnsigned(cursor, end(), out);
}

inline bool ProcFile::findByteCount(std::string_view key, std::uint64_t& out) const noexcept {
    const char* cursor = findValueFor(key);
    if (cursor == nullptr || !parseUnsigned(cursor, end(), out)) {
        return false;
    }

    std::uint64_t scale = 1;
    const char* const unit = skipSpaces(cursor, end());
    if (unit != end()) {
        switch (*unit) {
        case 'k':
        case 'K':
            scale = 1ull << 10;
            break;
        case 'm':
        case 'M':
            scale = 1ull << 20;
            break;
        case 'g':
        case 'G':
            scale = 1ull << 30;
            break;
        default:
            break;
        }
    }
    out *= scale;
    return true;
}

inline bool ProcFile::readStatField(std::size_t field, std::uint64_t& out) const noexcept {
    // Field 2 is the command name: parenthesised, and free to contain spaces, so
    // everything after it has to be located past the closing parenthesis.
    if (field < 3) {
        return false;
    }
    const char* closing = nullptr;
    for (std::size_t index = size_; index > 0; --index) {
        if (buffer_[index - 1] == ')') {
            closing = buffer_ + index - 1;
            break;
        }
    }
    if (closing == nullptr) {
        return false;
    }

    const char* cursor = skipSpaces(closing + 1, end());
    for (std::size_t index = 3; index < field; ++index) {
        while (cursor < end() && *cursor != ' ') {
            ++cursor;
        }
        cursor = skipSpaces(cursor, end());
    }
    return parseUnsigned(cursor, end(), out);
}

} // namespace detail

#endif // FRICTION_OS_LINUX

#if defined(FRICTION_OS_LINUX) || defined(FRICTION_OS_APPLE)

namespace detail {

/// `getrusage` splits CPU time into whole seconds and microseconds, so the two
/// have to be recombined and scaled to nanoseconds.
[[nodiscard]] inline std::uint64_t timevalToNanoseconds(const timeval& value) noexcept {
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(value.tv_usec) * 1'000ull;
}

/// Fills in the CPU time and page fault counters every POSIX platform provides.
/// Returns false when the kernel would not supply them.
[[nodiscard]] inline bool readResourceUsage(ProcessStats& stats) noexcept {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return false;
    }
    stats.userCpuNanoseconds = timevalToNanoseconds(usage.ru_utime);
    stats.systemCpuNanoseconds = timevalToNanoseconds(usage.ru_stime);
    stats.minorFaults = static_cast<std::uint64_t>(usage.ru_minflt);
    stats.majorFaults = static_cast<std::uint64_t>(usage.ru_majflt);
    return true;
}

} // namespace detail

#endif // FRICTION_OS_LINUX || FRICTION_OS_APPLE

[[nodiscard]] inline bool processStatsSupported() noexcept {
#if defined(FRICTION_OS_LINUX) || defined(FRICTION_OS_APPLE)
    return true;
#else
    return false;
#endif
}

[[nodiscard]] inline ProcessStats readProcessStats() noexcept {
    ProcessStats stats;

#if defined(FRICTION_OS_LINUX)
    // /proc/self/status carries memory and scheduling in one read; the field we
    // want for threads lives in /proc/self/stat, where it costs a second read.
    detail::ProcFile status;
    const bool haveStatus = status.open("/proc/self/status");
    if (haveStatus) {
        const bool sawResident = status.findByteCount("VmRSS:", stats.residentBytes);
        (void)status.findByteCount("VmHWM:", stats.peakResidentBytes);
        (void)status.findByteCount("VmSize:", stats.virtualBytes);
        (void)status.findUnsigned("voluntary_ctxt_switches:", stats.voluntaryContextSwitches);
        (void)status.findUnsigned("nonvoluntary_ctxt_switches:", stats.involuntaryContextSwitches);
        if (!sawResident) {
            stats.residentBytes = 0;
        }
    }

    detail::ProcFile stat;
    const bool haveStat = stat.open("/proc/self/stat");
    if (haveStat) {
        std::uint64_t threads = 0;
        if (stat.readStatField(20, threads)) {
            stats.threadCount = static_cast<std::uint32_t>(threads);
        }
    }

    const bool haveResourceUsage = detail::readResourceUsage(stats);
    if (haveStatus || haveStat) {
        stats.source = ProcessSource::procfs;
    } else {
        stats.source = haveResourceUsage ? ProcessSource::rusage : ProcessSource::unknown;
    }

#elif defined(FRICTION_OS_APPLE)
    stats.source =
        detail::readResourceUsage(stats) ? ProcessSource::rusage : ProcessSource::unknown;

#else
    stats.source = ProcessSource::unknown;
#endif

    return stats;
}

} // namespace friction
