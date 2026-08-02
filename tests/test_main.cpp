#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>

#include "test_harness.hpp"

// Test entry point — compiled into EVERY per-area test binary
// (bezel_tests_<group>; see tests/CMakeLists.txt). Each binary has its own
// registry holding only its own area's tests, so everything below is scoped to
// "this binary's tests"; the union over the binaries is the whole suite and
// tests/run_parallel.sh does the cross-binary accounting.
//
// Default behaviour is unchanged: run every registered test serially and print
// "N passed, M failed, T total". Four env vars opt into other modes:
//
//   BEZEL_TEST_FILTER=<substr>  — only run tests whose suite or name contains
//                                 the substring (delegates to the same matching
//                                 the harness has always used).
//   BEZEL_TEST_LIST=<path>      — only run tests exactly named by the file: one
//                                 `suite.name` per line (blank lines ignored,
//                                 CRLF tolerated). Composes with the filter
//                                 (both must match). The summary then carries an
//                                 explicit ", N selected" field — the integrity
//                                 basis for subset accounting in run_parallel.sh
//                                 (the gate canary tier, #2100): under a subset,
//                                 "every SELECTED test ran" replaces "every
//                                 REGISTERED test ran" as the drop-detection
//                                 contract. Unset → behaviour is byte-identical
//                                 to before (full-suite drop-detection intact).
//   BEZEL_SHARD_COUNT=<n>       — split the (optionally filtered) tests into n
//   BEZEL_SHARD_INDEX=<i>         disjoint shards and run only shard i (0-based).
//   BEZEL_TEST_ENUMERATE=1      — print the `suite.name` of every test this
//                                 process WOULD run (list + filter + shard
//                                 selection all applied), one per line, and
//                                 exit 0 without running any. Costs ~30 ms, so
//                                 scripts/agent_check.sh uses it to find which
//                                 binaries a filter matches before spending a
//                                 capped test run on them, and it is the
//                                 mechanical proof that the per-binary and
//                                 per-shard partitions have no overlap and no
//                                 gap (concatenate every binary's enumeration
//                                 and compare against the whole registry).
//
// Sharding lets a parallel driver (tests/run_parallel.sh) launch n copies of
// this binary across cores; each test process is self-contained (its own LLVM
// context, in-memory JIT) and peaks at ~314 MB, so a 16-way split runs the full
// suite in a fraction of the wall-clock time while still executing every test.
// The loop here mirrors bezel::test::runAll() so the shard variant lives in this
// single TU — editing it does not recompile the test translation units that
// include the harness header.
int main() {
    const char* filterEnv = std::getenv("BEZEL_TEST_FILTER");
    const std::string filter = filterEnv ? filterEnv : "";

    // Exact-name list selection (#2100). A missing/unreadable list file is a
    // hard error (exit 2), not an empty selection: a caller that asked for a
    // subset must never silently get the whole suite (or nothing).
    const char* listEnv = std::getenv("BEZEL_TEST_LIST");
    const bool listMode = listEnv && *listEnv;
    std::unordered_set<std::string> wanted;
    if (listMode) {
        std::ifstream in(listEnv);
        if (!in) {
            std::cerr << "bezel_tests: BEZEL_TEST_LIST file not readable: "
                      << listEnv << "\n";
            return 2;
        }
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() &&
                   (line.back() == '\r' || line.back() == ' ' ||
                    line.back() == '\t'))
                line.pop_back();
            const std::size_t b = line.find_first_not_of(" \t");
            if (b == std::string::npos) continue;
            wanted.insert(line.substr(b));
        }
    }

    auto envInt = [](const char* name, long fallback) -> long {
        const char* v = std::getenv(name);
        if (!v || !*v) return fallback;
        char* end = nullptr;
        long n = std::strtol(v, &end, 10);
        return (end && *end == '\0') ? n : fallback;
    };
    const char* enumEnv = std::getenv("BEZEL_TEST_ENUMERATE");
    const bool enumerateOnly = enumEnv && *enumEnv && std::string(enumEnv) != "0";

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
        // List selection first (exact suite.name), then the substring filter —
        // both must pass. Only the survive-predicate changes here; the
        // round-robin block below is untouched, so shard balance over the
        // selected set is preserved bit-for-bit (#2100).
        if (listMode &&
            wanted.count(test.suite + "." + test.name) == 0) {
            ++skipped;
            continue;
        }
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

        // Enumeration mode: report exactly what this process would run, then
        // move on. Placed AFTER the shard predicate on purpose — the output is
        // the partition proof, so it must reflect the shard slice too.
        if (enumerateOnly) {
            std::cout << test.suite << "." << test.name << "\n";
            continue;
        }

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

    // Enumeration is a pure listing: no summary line, so the output is
    // machine-clean (one `suite.name` per line and nothing else).
    if (enumerateOnly) return 0;

    // NOTE (per-area split): the phantom-name warning that used to live here —
    // "a BEZEL_TEST_LIST name matching NO registered test is a silently-dropped
    // test" (#2100) — moved to tests/run_parallel.sh. With the suite split
    // across several binaries no single binary can tell a phantom name from a
    // name that legitimately lives in a SIBLING binary: each would warn about
    // every test it does not own. The runner sums the per-binary `selected`
    // counts below and compares that GLOBAL total against the list, which is
    // the check the warning was always meant to be. The warning text
    // ("WARNING: only N of M listed tests matched the registry") is preserved
    // verbatim there, so the gate/agent_check tripwires that grep for it are
    // unaffected.

    std::cout << "\n" << passed << " passed, " << failed << " failed";
    if (skipped > 0) std::cout << ", " << skipped << " filtered";
    // The explicit selected-count field run_parallel.sh keys subset integrity
    // on (#2100). Only in list mode, so the full-suite summary stays
    // byte-identical to before.
    if (listMode) std::cout << ", " << selected << " selected";
    if (sharded)
        std::cout << " (shard " << shardIndex << "/" << shardCount << ")";
    std::cout << ", " << ::bezel::test::registry().size() << " total\n";
    return failed == 0 ? 0 : 1;
}
