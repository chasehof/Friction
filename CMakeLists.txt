cmake_minimum_required(VERSION 3.20)

project(
    Friction
    VERSION 0.1.0
    DESCRIPTION "A high-frequency telemetry dashboard for process metrics and observability"
    HOMEPAGE_URL "https://github.com/yourusername/friction"
    LANGUAGES CXX
)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(FRICTION_BUILD_TESTS "Build unit tests" ON)
option(FRICTION_INSTALL "Enable installation targets" ON)

# ============================================================================
# Library Definition
# ============================================================================

add_library(Friction INTERFACE)
add_library(Friction::Friction ALIAS Friction)

# Include directories
target_include_directories(
    Friction
    INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Public header files
set_target_properties(
    Friction
    PROPERTIES
    PUBLIC_HEADER "include/LockFreeQueue.hpp"
)

# C++ features
target_compile_features(Friction INTERFACE cxx_std_20)

# ============================================================================
# Installation
# ============================================================================

if(FRICTION_INSTALL)
    include(GNUInstallDirs)

    # Install headers
    install(
        FILES include/LockFreeQueue.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    # Install library targets
    install(
        TARGETS Friction
        EXPORT FrictionTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    # Install CMake config files
    install(
        EXPORT FrictionTargets
        FILE FrictionTargets.cmake
        NAMESPACE Friction::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Friction
    )

    # Create and install config files
    include(CMakePackageConfigHelpers)
    
    configure_package_config_file(
        "FrictionConfig.cmake.in"
        "FrictionConfig.cmake"
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