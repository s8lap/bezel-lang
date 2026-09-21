#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

#include <fcntl.h>

#include <fstream>
#include <iostream>
#include <unordered_set>

#include "native_test_harness.hpp"
#include "support/aot_support.hpp"
#include "support/force_sanitize.hpp"
#include "support/scratch_dir.hpp"
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
//   BEZEL_SHARD_SUMMARY=<path>  — ALSO write a structured JSON shard summary to
//                                 <path> (#3012 Phase 2a; additive, off by
//                                 default). The legacy summary line above is
//                                 unchanged and stays authoritative; the JSON
//                                 is the foundation for the gate reading
//                                 structured data (#3012 Phase 2c). Off when
//                                 unset → behaviour is byte-identical to before.
//
// Sharding lets a parallel driver (tests/run_parallel.sh) launch n copies of
// this binary across cores; each test process is self-contained (its own LLVM
// context, in-memory JIT) and peaks at ~314 MB, so a 16-way split runs the full
// suite in a fraction of the wall-clock time while still executing every test.
// The loop here mirrors bezel::test::runAll() so the shard variant lives in this
// single TU — editing it does not recompile the test translation units that
// include the harness header.

// ── per #3012 Phase 2a: structured shard-summary (additive, off by default) ──
// When BEZEL_SHARD_SUMMARY=<path> is set, main() ALSO writes a JSON object
// there (the fields run_parallel.sh greps out of the legacy line below). The
// legacy "N passed, M failed (shard i/n), T total" stderr line is UNCHANGED and
// remains the gate's authoritative source; the JSON is the foundation for
// Phase 2c (switch the gate's reader once N gates prove agreement). Off unless
// the env is set, so the default build is byte-identical to before.

// Minimal JSON string escape — the only string field is the binary path
// (argv[0]). Escapes the characters that would break the document; printable
// ASCII paths pass through unchanged.
static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

// Atomically install `content` at `path`: write a temp in the SAME directory
// (so the rename is one-filesystem), fsync the temp's data, then rename it into
// place (atomic on POSIX). Mirrors scripts/gate/io.py's atomic_json_write and
// aot_support.hpp's durableRename/atomicWriteStats — the #3000/#3003/#3009
// invariant: a reader either sees the whole new document or the prior state,
// never a partial write. Best-effort on any I/O failure (no file left).
static void atomicWriteShardSummary(const std::string& path,
                                    const std::string& content) {
    const std::size_t slash = path.find_last_of('/');
    const std::string dir = (slash == std::string::npos)
                                ? std::string(".") : path.substr(0, slash);
    const std::string base = (slash == std::string::npos)
                                 ? path : path.substr(slash + 1);
    // Unique temp name per process + per call (a process can emit at most once,
    // but the counter keeps two writers in the same dir from colliding).
    static std::atomic<unsigned long long> ctr{0};
    const std::string tmp = dir + "/" + base + ".tmp." +
                            std::to_string(::getpid()) + "." +
                            std::to_string(
                                ctr.fetch_add(1, std::memory_order_relaxed));
    const int fd = ::open(tmp.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    bool ok = true;
    std::size_t off = 0;
    while (off < content.size()) {
        const ssize_t n = ::write(fd, content.data() + off,
                                  content.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        off += static_cast<std::size_t>(n);
    }
    if (ok) {
        // Durability: fsync the temp's DATA before the rename so a crash between
        // the byte copy and the rename cannot install a truncated document
        // (#3009 — the load-bearing half of durableRename).
        while (::fsync(fd) != 0 && errno == EINTR) {}
    }
    ::close(fd);
    if (!ok) { ::unlink(tmp.c_str()); return; }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return;
    }
    // Best-effort directory fsync (some filesystems refuse; harmless — the data
    // fsync above is what prevents a truncated entry).
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        while (::fsync(dfd) != 0 && errno == EINTR) {}
        ::close(dfd);
    }
}

int main(int argc, char** argv) {
    // ── Point TMPDIR at the disk-backed scratch base, before any test runs ──
    // 247 test TUs place their temp files with std::filesystem::temp_directory_path(),
    // which is TMPDIR-or-/tmp — and /tmp here is a 16 GB tmpfs (RAM) with a
    // per-user quota. Those files leaked: on 2026-08-24 the accumulated debris
    // hit the 12,774 MB quota, every write to /tmp began failing EDQUOT, and
    // that wedged the session tooling itself for three sessions. Rather than
    // edit 247 call sites (and trust every future one to remember), set the
    // variable the standard function reads, once, in the entry point every
    // per-area test binary shares. temp_directory_path() is not cached, so
    // setting it here covers every later call — and child processes the
    // harness forks (clang, ld) inherit it, keeping their temporaries off the
    // tmpfs too. CLAUDE.md: scratch must never live in /tmp.
    //
    // overwrite=0: an operator who exported TMPDIR deliberately keeps it.
    {
        std::error_code ec;
        const std::string& base = bezel::test::aot_detail::scratchBase();
        std::filesystem::create_directories(base, ec);
        if (!ec) ::setenv("TMPDIR", base.c_str(), /*overwrite=*/0);
    }

    // Wall-clock start for the structured summary's elapsed_s field (#3012
    // Phase 2a). Captured before any test work so it reflects the whole shard.
    const auto t0 = std::chrono::steady_clock::now();

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
        // fix #4533: BRACKET THE BODY ON fd 2, NOT ON stdout.
        //
        // The gate hard-fails a cycle on a fired generated-code sanitizer
        // report, and to do that soundly it has to know WHICH TEST produced
        // one: a row that deliberately runs an undefined-behaviour program
        // (§28.2 use-after-dispose unchecked) gets a report AND passes, while
        // the same report in any other row is a memory-safety defect.
        //
        // That question cannot be answered from the `[ RUN  ]` lines above.
        // They go to `std::cout`, which is FULLY BUFFERED when a shard is
        // redirected into a file, while the sanitizer report is written to
        // unbuffered fd 2 (aot_support.hpp's emitSanitizerReportToStderr) — so
        // the two streams interleave in the log in an order that has nothing to
        // do with the order of execution. Attribution by position is wrong, and
        // wrong in a way that LOOKS right: it names a plausible neighbour.
        //
        // These markers are on the SAME unbuffered stream as the report, so
        // their relative order is exactly the execution order and a report
        // between `enter X` and `leave X` belongs to X. Only under the sweep,
        // where reports can actually occur — a normal single-binary run's
        // stderr stays byte-for-byte as it was. The spelling deliberately does
        // not start with `[ RUN`: scripts/gate/summary.py's crashed-shard
        // culprit extraction is anchored on that prefix (#1608).
        const bool sanScope = ::bezel::test::forceSanitizeAddress();
        if (sanScope) {
            // Flush the buffered `[ RUN  ]` line first, so that UNDER THE SWEEP
            // the two streams also read in order for a human — the gate prints
            // excerpts of these logs to an operator. Deliberately only here: on
            // the plain oracle the log stays byte-for-byte as it was, which
            // keeps summary.py's documented #1608 last-[ RUN ]-line culprit
            // behaviour (and its buffering quirks) exactly as consumers expect.
            std::cout.flush();
            std::fprintf(stderr, "[san-scope] enter %s.%s\n",
                         test.suite.c_str(), test.name.c_str());
            std::fflush(stderr);
        }
        test.body();
        const bool testFailed = ::bezel::test::currentTestFailed();
        if (sanScope) {
            std::fprintf(stderr, "[san-scope] leave %s.%s %s\n",
                         test.suite.c_str(), test.name.c_str(),
                         testFailed ? "fail" : "pass");
            std::fflush(stderr);
        }
        if (testFailed) {
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

    // fix #3626: loud, not silent. Every one of the ~122 test files that need
    // the native ASan/TSan runtime object funnels through
    // native_test_harness.hpp's sanitizerRuntimeMaster() (via
    // asanRuntimeObject() / buildRuntimeSanitizerObject()); when it is
    // unavailable those tests used to skip invisibly and still report a
    // clean PASS. Read the ONE process-wide counter here — the harness's own
    // chokepoint — and print it UNCONDITIONALLY, so "0" is a visible
    // confirmation the oracle actually ran this process rather than the
    // absence of a signal. A separate line (not embedded in the "N passed,
    // M failed ... T total" summary above) so the LOAD-BEARING legacy-line
    // regex (scripts/gate/summary.py's _LEGACY_HEAD_RE/_LEGACY_TOTAL_RE,
    // which tests/run_parallel.sh's dual-read agreement check depends on)
    // is untouched.
    const std::size_t nativeSanitizerSkips =
        ::bezel::native_test::nativeSanitizerUnavailableSkips().load(
            std::memory_order_relaxed);
    std::cout << "NATIVE SANITIZER: " << nativeSanitizerSkips
              << " check(s) skipped (ASan/TSan runtime unavailable — see"
                 " tests/native_test_harness.hpp sanitizerRuntimeMaster())\n";

    // fix #3733: loud, not silent. runAotChecked's toolchain probe at
    // tests/support/aot_support.hpp takes a single early-return when
    // `clang --version` fails (or BEZEL_AOT_FORCE_NO_TOOLCHAIN=1 forces it).
    // That branch was previously the #3633 silent-skip class: a box without
    // clang reported clean PASS having run no sanitizer at all. The counter
    // `checkedToolchainSkips()` is incremented at that ONE chokepoint
    // (modelled on nativeSanitizerUnavailableSkips above), and is printed
    // here UNCONDITIONALLY — "0" is then a positive confirmation the
    // ASan/LSan tier was actually armed in this process, not the absence of
    // a signal. A separate line (not embedded in the "N passed, M failed …
    // T total" summary above) so the LOAD-BEARING legacy-line regex
    // (scripts/gate/summary.py's _LEGACY_HEAD_RE/_LEGACY_TOTAL_RE, which
    // tests/run_parallel.sh's dual-read agreement check depends on) is
    // untouched.
    const std::size_t checkedToolchainSkips =
        ::bezel::test::checkedToolchainSkips().load(std::memory_order_relaxed);
    std::cout << "CHECKED TOOLCHAIN: " << checkedToolchainSkips
              << " skip(s) (clang unavailable for the ASan link — see"
                 " tests/support/aot_support.hpp runAotCheckedImpl)\n";

    // per #3012 Phase 2a: ALSO emit a structured JSON shard summary to the path
    // named by BEZEL_SHARD_SUMMARY (off unless set). The legacy summary line
    // above is UNCHANGED and remains the gate's authoritative source; this JSON
    // is the foundation for Phase 2c (switch the reader once N gates prove
    // agreement, observation-only until then). Atomic (temp + fsync + rename)
    // so run_parallel's agreement check never reads a partial document.
    const char* sumEnv = std::getenv("BEZEL_SHARD_SUMMARY");
    if (sumEnv && *sumEnv) {
        const int rc = (failed == 0) ? 0 : 1;
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        const char* bin = (argc > 0 && argv[0]) ? argv[0] : "";
        // #1822: generated-code ASan is forced when BEZEL_AOT_FORCE_SANITIZE is
        // address/asan/1 (the --full oracle). This used to be a hand-inlined
        // copy of that parse that "mirrors forceSanitizeAddress" by comment
        // only; it now IS forceSanitizeAddress, so the field this shard summary
        // records cannot disagree with the bit the harness actually applied.
        const bool asan = ::bezel::test::forceSanitizeAddress();
        char elapsedBuf[32];
        std::snprintf(elapsedBuf, sizeof(elapsedBuf), "%.3f", elapsed);
        std::string json = "{\"schema\":\"bezel.shard-summary.v1\",";
        json += "\"binary\":\"";
        json += jsonEscape(bin);
        json += "\",\"shard_index\":";
        json += std::to_string(shardIndex);
        json += ",\"shard_count\":";
        json += std::to_string(shardCount);
        json += ",\"passed\":";
        json += std::to_string(passed);
        json += ",\"failed\":";
        json += std::to_string(failed);
        json += ",\"total\":";
        json += std::to_string(::bezel::test::registry().size());
        json += ",\"selected\":";
        json += std::to_string(selected);
        json += ",\"asan_mode\":";
        json += asan ? "true" : "false";
        json += ",\"rc\":";
        json += std::to_string(rc);
        json += ",\"elapsed_s\":";
        json += elapsedBuf;
        // fix #3626: additive field — an older reader (summary.py's
        // check_agreement only compares passed/failed/total/selected/schema)
        // ignores an unknown key, so this needs no schema-version bump.
        // tests/run_parallel.sh sums it across every shard's JSON directly
        // (grep/awk, not summary.py) and fails the sharded run when the total
        // is nonzero.
        json += ",\"native_sanitizer_unavailable_skips\":";
        json += std::to_string(nativeSanitizerSkips);
        // fix #3733: additive field — same rationale as #3626 above; an older
        // reader that does not know this key ignores it (summary.py's
        // check_agreement only compares passed/failed/total/selected/schema),
        // so no schema-version bump is needed. tests/run_parallel.sh sums it
        // across every shard's JSON directly and fails the sharded run when
        // the total is nonzero (GATE FAILURE banner — the loud half of the
        // #3733 toolchain-SKIP ratchet).
        json += ",\"checked_toolchain_skips\":";
        json += std::to_string(checkedToolchainSkips);
        json += "}\n";
        atomicWriteShardSummary(std::string(sumEnv), json);
    }

    return failed == 0 ? 0 : 1;
}
