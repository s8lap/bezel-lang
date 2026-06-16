#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>

#include "test_harness.hpp"

// Test entry point.
//
// Default behaviour is unchanged: run every registered test serially and print
// "N passed, M failed, T total". Two env vars opt into faster runs:
//
//   BEZEL_TEST_FILTER=<substr>  — only run tests whose suite or name contains
//                                 the substring (delegates to the same matching
//                                 the harness has always used).
//   BEZEL_SHARD_COUNT=<n>       — split the (optionally filtered) tests into n
//   BEZEL_SHARD_INDEX=<i>         disjoint shards and run only shard i (0-based).
//
// Sharding lets a parallel driver (tests/run_parallel.sh) launch n copies of
// this binary across cores; each test process is self-contained (its own LLVM
// context, in-memory JIT) and peaks at ~314 MB, so a 16-way split runs the full
// suite in a fraction of the wall-clock time while still executing every test.
// The loop here mirrors bezel::test::runAll() so the shard variant lives in this
// single TU — editing it does not recompile the 574 test translation units that
// include the harness header.
int main() {
    const char* filterEnv = std::getenv("BEZEL_TEST_FILTER");
    const std::string filter = filterEnv ? filterEnv : "";

    auto envInt = [](const char* name, long fallback) -> long {
        const char* v = std::getenv(name);
        if (!v || !*v) return fallback;
        char* end = nullptr;
        long n = std::strtol(v, &end, 10);
        return (end && *end == '\0') ? n : fallback;
    };
    long shardCount = envInt("BEZEL_SHARD_COUNT", 1);
    long shardIndex = envInt("BEZEL_SHARD_INDEX", 0);
    if (shardCount < 1) shardCount = 1;
    if (shardIndex < 0 || shardIndex >= shardCount) shardIndex = 0;
    const bool sharded = shardCount > 1;

    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    // Index over the tests that survive the filter, so shards stay balanced
    // regardless of how many tests the filter removes.
    std::size_t selected = 0;
    for (const ::bezel::test::TestCase& test : ::bezel::test::registry()) {
        if (!filter.empty()) {
            bool match = test.suite.find(filter) != std::string::npos ||
                         test.name.find(filter) != std::string::npos;
            if (!match) { ++skipped; continue; }
        }
        // Round-robin assignment of filter-surviving tests to shards.
        const bool mine = (selected % static_cast<std::size_t>(shardCount)) ==
                          static_cast<std::size_t>(shardIndex);
        ++selected;
        if (!mine) continue;

        ::bezel::test::currentTestFailed() = false;
        std::cout << "[ RUN  ] " << test.suite << "." << test.name << "\n";
        test.body();
        if (::bezel::test::currentTestFailed()) {
            ++failed;
            std::cout << "[ FAIL ] " << test.suite << "." << test.name << "\n";
        } else {
            ++passed;
            std::cout << "[  OK  ] " << test.suite << "." << test.name << "\n";
        }
    }

    std::cout << "\n" << passed << " passed, " << failed << " failed";
    if (skipped > 0) std::cout << ", " << skipped << " filtered";
    if (sharded)
        std::cout << " (shard " << shardIndex << "/" << shardCount << ")";
    std::cout << ", " << ::bezel::test::registry().size() << " total\n";
    return failed == 0 ? 0 : 1;
}
