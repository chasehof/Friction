cmake_minimum_required(VERSION 3.20)

project(
    Friction
    VERSION 0.1.0
    DESCRIPTION "A high-frequency telemetry dashboard for process metrics and observability"
    HOMEPAGE_URL "https://github.com/chasehof/Friction"
    LANGUAGES CXX
)

# Project-wide settings
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(FRICTION_BUILD_TESTS "Build unit tests" ON)
option(FRICTION_INSTALL "Enable installation targets" ON)

# ============================================================================
# Main Library Target
# ============================================================================

add_library(Friction
    src/RingBuffer.cpp
    src/Telemetry.cpp
)

add_library(Friction::Friction ALIAS Friction)

target_include_directories(
    Friction
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(Friction PUBLIC cxx_std_20)

# ============================================================================
# Installation
# ============================================================================

if(FRICTION_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    # Install library
    install(
        TARGETS Friction
        EXPORT FrictionTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    # Install headers
    install(
        DIRECTORY include/friction
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp"
    )

    # Install CMake config files
    install(
        EXPORT FrictionTargets
        FILE FrictionTargets.cmake
        NAMESPACE Friction::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Friction
    )

    configure_package_config_file(
        FrictionConfig.cmake.in
        FrictionConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Friction
    )

    write_basic_package_version_file(
        FrictionConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(
        FILES
            ${CMAKE_CURRENT_BINARY_DIR}/FrictionConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/FrictionConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Friction
    )
endif()

# ============================================================================
# Testing
# ============================================================================

if(FRICTION_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()