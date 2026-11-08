#pragma once

///
/// \file platform.hpp
///
/// Compiler, architecture and operating system detection.
///
/// Every Friction detail header includes this first and may rely on the macros
/// defined here. Exactly one `FRICTION_COMPILER_*` and one `FRICTION_OS_*` macro
/// is defined; architecture macros are only defined for the architecture that
/// actually provides an accelerated clock.
///
///
#include <cstddef>

// ---------------------------------------------------------------- compiler --

#if defined(_MSC_VER)
#define FRICTION_COMPILER_MSVC 1
#elif defined(__clang__)
#define FRICTION_COMPILER_CLANG 1
#elif defined(__GNUC__)
#define FRICTION_COMPILER_GCC 1
#else
#define FRICTION_COMPILER_UNKNOWN 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#define FRICTION_LIKELY(x) __builtin_expect(!!(x), 1)
#define FRICTION_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define FRICTION_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define FRICTION_LIKELY(x) (!!(x))
#define FRICTION_UNLIKELY(x) (!!(x))
#define FRICTION_NOINLINE __declspec(noinline)
#else
#define FRICTION_LIKELY(x) (!!(x))
#define FRICTION_UNLIKELY(x) (!!(x))
#define FRICTION_NOINLINE
#endif

// ------------------------------------------------------------ architecture --

#if defined(__x86_64__) || defined(_M_X64)
#define FRICTION_ARCH_X86_64 1
#elif defined(__i386__) || defined(_M_IX86)
#define FRICTION_ARCH_X86_32 1
#endif

#if defined(FRICTION_ARCH_X86_64) || defined(FRICTION_ARCH_X86_32)
#if defined(FRICTION_COMPILER_MSVC)
#include <intrin.h>
#endif
#endif

// --------------------------------------------------------------------- os --

#if defined(__linux__)
#define FRICTION_OS_LINUX 1
#elif defined(__APPLE__)
#define FRICTION_OS_APPLE 1
#elif defined(_WIN32)
#define FRICTION_OS_WINDOWS 1
#endif

namespace friction::detail {

/// Bytes in a cache line. Counters are padded to this to keep unrelated
/// counters from sharing a line.
inline constexpr std::size_t kCacheLineSize = 64;

/// Hint to the CPU that we are in a spin-wait loop.
inline void cpuRelax() noexcept {
#if defined(FRICTION_ARCH_X86_64) || defined(FRICTION_ARCH_X86_32)
#if defined(FRICTION_COMPILER_MSVC)
    _mm_pause();
#else
    __builtin_ia32_pause();
#endif
#endif
}

} // namespace friction::detail