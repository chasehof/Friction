# Tests for Friction.
#
# The suite builds twice: once against the modular headers, and once against the
# generated single header. If the amalgamation ever stops being a faithful
# translation of the sources, the second build is what notices.

set(FRICTION_TEST_SOURCES
    test_clock.cpp
    test_counters.cpp
    test_histogram.cpp
    test_main.cpp
    test_overhead.cpp
    test_process.cpp
    test_report.cpp
    test_session.cpp
)

set(FRICTION_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wold-style-cast
    -Wcast-qual
    -Wdouble-promotion
    -Wformat=2
)

add_executable(friction_tests ${FRICTION_TEST_SOURCES})
target_link_libraries(friction_tests PRIVATE Friction::Friction)
target_include_directories(friction_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(friction_tests PRIVATE /W4 /permissive-)
    if(FRICTION_WERROR)
        target_compile_options(friction_tests PRIVATE /WX)
    endif()
else()
    target_compile_options(friction_tests PRIVATE ${FRICTION_WARNINGS})
    if(FRICTION_WERROR)
        target_compile_options(friction_tests PRIVATE -Werror)
    endif()
endif()

add_test(NAME friction_tests COMMAND friction_tests)

# The same suite, against the header a user would paste into a project.
add_executable(friction_single_include_tests ${FRICTION_TEST_SOURCES})
target_include_directories(
    friction_single_include_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/single_include
)
target_compile_features(friction_single_include_tests PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(friction_single_include_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(friction_single_include_tests PRIVATE ${FRICTION_WARNINGS})
endif()

# `<friction/friction.hpp>` resolves to single_include/friction/friction.hpp here,
# which is the include a drop-in user writes.
add_test(NAME friction_single_include_tests COMMAND friction_single_include_tests)

if(Python3_Interpreter_FOUND)
    add_test(
        NAME friction_amalgamation_is_current
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/amalgamate.py --check
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
endif()