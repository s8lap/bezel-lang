#pragma once

// Minimal zero-dependency test harness.
//
// Adopting a third-party test framework (GoogleTest, Catch2, doctest) is a
// project-level decision — dependencies are debt (CLAUDE.md) — that has been
// deferred. Until it is made, tests use this small harness: register a test
// with BEZEL_TEST, assert with the BEZEL_EXPECT_* macros, and let test_main.cpp
// run them. The macro surface is intentionally small so that migrating to a
// real framework later is cheap.

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace bezel::test {

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

// Function-local static avoids static-initialization-order issues across TUs.
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

// Set by the BEZEL_EXPECT_* macros when an assertion in the current test fails.
inline bool& currentTestFailed() {
    static bool failed = false;
    return failed;
}

struct Registrar {
    Registrar(std::string suite, std::string name, std::function<void()> body) {
        registry().push_back(
            TestCase{std::move(suite), std::move(name), std::move(body)});
    }
};

// Run all registered tests, optionally filtered by BEZEL_TEST_FILTER.
// If the env var is set, only tests whose suite or name contains the substring
// (case-sensitive) are run; unmatched tests are silently skipped. If unset,
// all tests are run (unchanged behaviour).
inline int runAll() {
    const char* filterEnv = std::getenv("BEZEL_TEST_FILTER");
    std::string filter = filterEnv ? filterEnv : "";

    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    for (const TestCase& test : registry()) {
        if (!filter.empty()) {
            bool match = test.suite.find(filter) != std::string::npos ||
                         test.name.find(filter) != std::string::npos;
            if (!match) { ++skipped; continue; }
        }
        currentTestFailed() = false;
        std::cout << "[ RUN  ] " << test.suite << "." << test.name << "\n";
        test.body();
        if (currentTestFailed()) {
            ++failed;
            std::cout << "[ FAIL ] " << test.suite << "." << test.name << "\n";
        } else {
            ++passed;
            std::cout << "[  OK  ] " << test.suite << "." << test.name << "\n";
        }
    }
    std::cout << "\n"
              << passed << " passed, " << failed << " failed";
    if (skipped > 0) std::cout << ", " << skipped << " filtered";
    std::cout << ", " << registry().size() << " total\n";
    return failed == 0 ? 0 : 1;
}

} // namespace bezel::test

// Register a test. Usage:
//   BEZEL_TEST(suite_name, test_name) { BEZEL_EXPECT_TRUE(...); }
#define BEZEL_TEST(suite, name)                                                \
    static void bezel_test_##suite##_##name();                                 \
    static const ::bezel::test::Registrar bezel_registrar_##suite##_##name(    \
        #suite, #name, bezel_test_##suite##_##name);                           \
    static void bezel_test_##suite##_##name()

#define BEZEL_EXPECT_TRUE(cond)                                                \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ::bezel::test::currentTestFailed() = true;                         \
            std::cout << "    assertion failed: " #cond " (" << __FILE__ << ":"\
                      << __LINE__ << ")\n";                                    \
        }                                                                      \
    } while (false)

#define BEZEL_EXPECT_FALSE(cond) BEZEL_EXPECT_TRUE(!(cond))

#define BEZEL_EXPECT_EQ(lhs, rhs)                                              \
    do {                                                                       \
        if (!((lhs) == (rhs))) {                                               \
            ::bezel::test::currentTestFailed() = true;                         \
            std::cout << "    expected equality: " #lhs " == " #rhs " ("       \
                      << __FILE__ << ":" << __LINE__ << ")\n";                 \
        }                                                                      \
    } while (false)
