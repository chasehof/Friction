///
/// \file test_main.cpp
///
/// Test entry point. Pass a substring on the command line to run a subset, for
/// example `friction_tests histogram`.
///
#include "harness.hpp"

#include <string>

int main(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    return friction::test::runAll(filter);
}
