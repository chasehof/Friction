cmake_minimum_required(VERSION 3.20)

project(
    Friction
    VERSION 0.1.0
    DESCRIPTION "Low overhead performance measurement for C++ programs"
    HOMEPAGE_URL "https://github.com/chasehof/Friction"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FRICTION_BUILD_TESTS "Build the test suite" ON)
option(FRICTION_INSTALL "Enable installation targets" ON)
option(FRICTION_WERROR "Treat warnings in the test suite as errors" OFF)

# Needed before the library target is declared: the install interface quotes
# CMAKE_INSTALL_INCLUDEDIR, and an undefined variable there would export an empty
# include path rather than an error.
include(GNUInstallDirs)

find_package(Python3 COMPONENTS Interpreter)

# ============================================================================
# The library
#
# Friction is header-only, so the target carries usage requirements and nothing
# else. Consumers get include paths and a C++20 requirement; there is nothing to
# link, which is the point.
# ============================================================================

add_library(Friction INTERFACE)
add_library(Friction::Friction ALIAS Friction)

target_include_directories(
    Friction
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(Friction INTERFACE cxx_std_20)

# ============================================================================
# Generated single-header distribution
#
# single_include/friction/friction.hpp is generated from the modular headers so
# users who want exactly one file can have one. It is checked in, because a
# header that has to be built before it can be consumed is not a drop-in.
# ============================================================================

set(FRICTION_AMALGAMATED_HEADER
    ${CMAKE_CURRENT_SOURCE_DIR}/single_include/friction/friction.hpp
)

if(Python3_Interpreter_FOUND)
    add_custom_target(
        friction_amalgamate
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/amalgamate.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating single_include/friction/friction.hpp"
        VERBATIM
    )

    add_custom_target(
        friction_verify_amalgamation
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/amalgamate.py --check
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Checking that single_include/friction/friction.hpp is up to date"
        VERBATIM
    )
else()
    message(STATUS "Friction: python3 not found, the amalgamation targets are unavailable")
endif()

# ============================================================================
# Installation
# ============================================================================

if(FRICTION_INSTALL)
    include(CMakePackageConfigHelpers)

    install(
        TARGETS Friction
        EXPORT FrictionTargets
    )

    # Both spellings are installed: the modular headers for projects that use the
    # whole library, and the single header for projects that would rather not.
    install(
        DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/friction
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp"
    )

    install(
        FILES ${FRICTION_AMALGAMATED_HEADER}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/friction/single_include
    )

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