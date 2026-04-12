# Friction

A high-frequency telemetry dashboard for process metrics and observability

## The Problem

Standard system monitors (top, htop, btop) provide a high-level view but miss the micro friction that can kill performance in applications. Profilers provide deep insight but can slow down execution by orders of magnitude and are typically difficult to set up.

Friction fills the gap: a low-overhead, real-time telemetry tool that monitors process-specific churn.

## Core Metrics

1. **Memory Velocity**: Tracking allocations/deallocations per second to identify heap churn
2. **Thread Contention**: Monitoring context switches and mutex wait times
3. **Syscall Frequency**: Identifying noisy I/O paths that trap into the kernel too often
4. **Data Locality**: Using PMCs to track L1/L2 cache misses

## Building

### Prerequisites

- CMake 3.20 or later
- C++20 compatible compiler (GCC 10+, Clang 10+, MSVC 2019+)
- Linux, macOS, or Windows

### Build Instructions

```bash
# Clone the repository
git clone https://github.com/yourusername/friction.git
cd friction

# Create build directory
mkdir build
cd build

# Configure with CMake
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build the project
cmake --build .

# (Optional) Run tests
ctest --output-on-failure

# (Optional) Install
cmake --install . --prefix /usr/local
```

### Build Options

You can customize the build with CMake options:

```bash
# Disable test building
cmake .. -DFRICTION_BUILD_TESTS=OFF

# Disable installation targets
cmake .. -DFRICTION_INSTALL=OFF

# Use a specific build type
cmake .. -DCMAKE_BUILD_TYPE=Debug
```

## License

See [LICENSE](LICENSE) for details.
