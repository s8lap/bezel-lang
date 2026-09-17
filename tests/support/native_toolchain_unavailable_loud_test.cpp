// Ratchet: an unavailable C toolchain (clang) for runAotChecked must be
// COUNTED and REPORTED, never silently absorbed as a per-test SKIP.
//
// WHY THIS TEST EXISTS. `runAotCheckedImpl` probes `clang --version` once per
// process and, on failure, takes a single early-return at
// tests/support/aot_support.hpp that emits the literal diagnostic
// "<name>: SKIP — C toolchain (clang) unavailable for the ASan link". On a
// box without clang the whole ASan/LSan tier then reports clean PASS having
// run no sanitizer at all — exactly the silent-skip defect #3733 names. The
// fix is a single counting chokepoint: the same shape as #3626's
// nativeSanitizerUnavailableSkips() (tests/native_test_harness.hpp:65–68),
// every one of the 2,046 runAotChecked( call sites in 634 files funnels
// through that one early-return, so counting there — not touching any of
// the 2,046 files — is what makes "reported, not silent" a harness property.
// tests/test_main.cpp prints the per-process tally unconditionally at the end
// of every run (the "CHECKED TOOLCHAIN: N skip(s)" line) and
// tests/run_parallel.sh sums it across every shard's structured JSON
// summary, failing the sharded run when the total is nonzero — the same
// floor shape as the #3626 native-sanitizer floor.
//
// This file pins the counting chokepoint itself, portably: it does not
// depend on reproducing the original "box has no clang" condition (which a
// CI/container may or may not have). The `BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`
// knob (read by `bezel::test::forceToolchainMissing()` in aot_support.hpp)
// makes the underlying probe genuinely fail — the same terminal outcome a
// missing/broken toolchain produces — with no environment tampering
// required, so this test is deterministic on every box regardless of
// whether clang is on PATH.
//
// If this test fails: check that runAotCheckedImpl's `toolchainOk` probe
// at tests/support/aot_support.hpp still funnels through the counted
// wrapper added by #3733, that the increment is on the ONE early-return
// path (the outer one — the six inner memoised early-returns of
// sanitizerRuntimeMaster are unrelated), and that the
// BEZEL_AOT_FORCE_NO_TOOLCHAIN test-only knob is honoured by the static
// const initialisation.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#include <sys/wait.h>

#include "support/aot_support.hpp"
#include "support/scratch_dir.hpp"
#include "test_harness.hpp"

namespace {

namespace aot = bezel::test::aot_detail;
using bezel::test::currentTestFailed;

// A trivial valid Bezel program: returns 0 from Main. Used by the in-child
// probe — its sole purpose is to drive runAotChecked past the toolchain
// probe so the SKIP branch (and therefore the counter increment) fires.
constexpr const char* kAnyProgram =
    "module M; class C public static method Main(): int "
    "  return 0; "
    "end Main; end C;";

// Belt and braces against recursion: the parent tests below re-exec this
// binary filtered to the in-child probe; this env var tells the child
// to skip the parent tests (which would otherwise spawn grandchildren
// indefinitely).
constexpr const char* kChildGuard = "BEZEL_AOT_NO_TOOLCHAIN_SELFTEST_CHILD";

std::string selfExe() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::string(buf);
}

std::string readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

// ---------------------------------------------------------------------------
// In-process probe: when BEZEL_AOT_FORCE_NO_TOOLCHAIN=1 is set at runtime,
// runAotChecked's toolchain probe returns the SKIP path and the process-wide
// counter `checkedToolchainSkips()` is incremented. Mirrors
// tests/support/native_sanitizer_unavailable_loud_test.cpp's first probe
// (the `unavailable_result_increments_the_counter` shape, with a
// test-only env var instead of a "bogus sanitizer" name).
// ---------------------------------------------------------------------------
BEZEL_TEST(native_toolchain_unavailable_loud,
           unavailable_result_increments_the_counter) {
    auto& counter = ::bezel::test::checkedToolchainSkips();
    const std::size_t saved = counter.load();

    // `forceToolchainMissing()` re-reads the env var on every call (it is
    // NOT memoized — the regression test sets it at runtime via setenv(), so
    // a memoised static const would miss it; production paths never set it
    // so the cost is irrelevant). Force it for this probe, then unset so a
    // subsequent test in the same process is unaffected.
    ::setenv("BEZEL_AOT_FORCE_NO_TOOLCHAIN", "1", 1);
    auto r = bezel::test::runAotChecked(kAnyProgram);
    ::unsetenv("BEZEL_AOT_FORCE_NO_TOOLCHAIN");

    // The SKIP branch fires: ran=false, hasErrors=true, message contains
    // the fixed "SKIP — C toolchain" literal.
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    if (!r.diagnostics.diagnostics().empty()) {
        const auto& d = r.diagnostics.diagnostics().front();
        BEZEL_EXPECT_TRUE(d.severity == bezel::Severity::Error);
        BEZEL_EXPECT_TRUE(d.message.find("SKIP") != std::string::npos);
        BEZEL_EXPECT_TRUE(d.message.find("C toolchain") != std::string::npos);
    }

    // The load-bearing assertion: the chokepoint counted.
    BEZEL_EXPECT_TRUE(counter.load() > saved);

    // Restore: this probe's DELIBERATE failure must not poison the
    // process-wide tally test_main.cpp prints at the end of THIS run, nor
    // the per-shard JSON tests/run_parallel.sh sums into its #3733 floor
    // check — it is a synthetic negative case pinning the mechanism, not a
    // real toolchain outage, and the harness has no way to tell the two
    // apart except this test putting the shared counter back where it found
    // it.
    counter.store(saved);
}

// ---------------------------------------------------------------------------
// In-child probe: invoked under BEZEL_AOT_FORCE_NO_TOOLCHAIN=1 so every
// runAotChecked row hits the SKIP branch. The assertion `BEZEL_EXPECT_TRUE
// (r.ran)` therefore FAILS — that is the FAIL-WITHOUT vehicle for the E2E
// parent tests below (they re-exec this binary filtered to this test name
// and observe a non-zero exit + a "CHECKED TOOLCHAIN: N skip(s)" line).
// ---------------------------------------------------------------------------
BEZEL_TEST(native_toolchain_unavailable_loud,
           child_probe_under_missing_toolchain) {
    auto r = bezel::test::runAotChecked(kAnyProgram);
    // The SKIP branch fires — r.ran is false. This assertion therefore FAILS
    // in the child. That is exactly what the E2E parent tests pin: the
    // child exits non-zero, prints "CHECKED TOOLCHAIN: 1 skip(s)", and the
    // shard JSON carries checked_toolchain_skips=1.
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// E2E (parent): under a forced-missing toolchain the child must FAIL the
// in-child probe — i.e. exit non-zero — and the per-process line
// "CHECKED TOOLCHAIN: N skip(s)" must be present with N > 0. This is the
// loudness guarantee: a box where the ASan/LSan tier is silently disarmed
// cannot report clean PASS.
//
// Mirrors tests/support/aot_infra_loud_3007_test.cpp's
// `unusable_scratch_base_fails_the_probe_loudly` (BEZEL_AOT_INFRA_SELFTEST_CHILD
// is the recursion guard there; here it is kChildGuard above).
// ---------------------------------------------------------------------------
BEZEL_TEST(native_toolchain_unavailable_loud,
           missing_toolchain_fails_child_loudly) {
    if (const char* c = std::getenv(kChildGuard); c && *c) return;

    const std::string exe = selfExe();
    BEZEL_EXPECT_FALSE(exe.empty());
    if (exe.empty()) return;

    const std::filesystem::path dir = wtScratchDir("toolchain-3733-loud");
    const std::string outPath = (dir / "child.out").string();

    bool timedOut = false;
    int execErrno = 0;
    const int status = aot::spawnCaptured(
        {exe}, outPath, outPath, /*timeoutSec=*/300, timedOut,
        {{"BEZEL_AOT_FORCE_NO_TOOLCHAIN", "1"},
         {"BEZEL_TEST_FILTER", "child_probe_under_missing_toolchain"},
         {kChildGuard, "1"},
         {"BEZEL_TEST_LIST", ""},
         {"BEZEL_TEST_ENUMERATE", ""},
         {"BEZEL_SHARD_COUNT", ""},
         {"BEZEL_SHARD_INDEX", ""},
         {"BEZEL_SHARD_SUMMARY", ""}},
        &execErrno);
    const std::string out = readAll(outPath);

    BEZEL_EXPECT_FALSE(timedOut);
    BEZEL_EXPECT_EQ(execErrno, 0);
    BEZEL_EXPECT_TRUE(status >= 0);

    // The child must have RUN the probe — a filter matching zero tests
    // prints "0 passed, 0 failed", which is indistinguishable from success
    // (LANE_RULES §2), so pin the match count before believing anything
    // else.
    BEZEL_EXPECT_TRUE(out.find("[ RUN  ] native_toolchain_unavailable_loud."
                               "child_probe_under_missing_toolchain") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("1 passed, 0 failed") == std::string::npos);

    // The in-child probe must have FAILED — proves the SKIP was loud, not
    // silent (a pre-#3733 harness returned ran=true-equivalent for a SKIP,
    // the assertion `BEZEL_EXPECT_TRUE(r.ran)` would have passed, and the
    // child would have exited 0). Pre-fix: this assertion is moot because
    // the earlier assertions (the "CHECKED TOOLCHAIN:" line below) already
    // pinned the loudness mechanism itself.
    BEZEL_EXPECT_TRUE(out.find("[ FAIL ] native_toolchain_unavailable_loud."
                               "child_probe_under_missing_toolchain") !=
                      std::string::npos);

    // The loudness line: the per-process tally test_main.cpp prints
    // unconditionally at exit. Pre-fix: this string never appears in the
    // output at all (the print was added by #3733), so the assertion
    // below is the FAIL-WITHOUT vehicle.
    BEZEL_EXPECT_TRUE(out.find("CHECKED TOOLCHAIN:") != std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("CHECKED TOOLCHAIN: 0") == std::string::npos);

    // Non-zero exit, so a runner that only reads rc still sees the failure
    // — same shape as the #3007 floor.
    BEZEL_EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) != 0);

    if (currentTestFailed())
        std::cout << "--- child output ---\n" << out << "\n--- end ---\n";
}

// ---------------------------------------------------------------------------
// E2E (parent): under a forced-missing toolchain the child's shard JSON
// summary must carry `checked_toolchain_skips` with a non-zero value. This
// is the per-shard JSON field that tests/run_parallel.sh sums into its #3733
// floor check.
//
// Pre-fix: the field is absent from the JSON, so this test fails. Post-fix:
// the field is present and non-zero.
// ---------------------------------------------------------------------------
BEZEL_TEST(native_toolchain_unavailable_loud,
           missing_toolchain_appends_to_shard_summary) {
    if (const char* c = std::getenv(kChildGuard); c && *c) return;

    const std::string exe = selfExe();
    BEZEL_EXPECT_FALSE(exe.empty());
    if (exe.empty()) return;

    const std::filesystem::path dir = wtScratchDir("toolchain-3733-shard");
    const std::string summaryPath = (dir / "shard.json").string();
    const std::string outPath = (dir / "child.out").string();

    bool timedOut = false;
    int execErrno = 0;
    const int status = aot::spawnCaptured(
        {exe}, outPath, outPath, /*timeoutSec=*/300, timedOut,
        {{"BEZEL_AOT_FORCE_NO_TOOLCHAIN", "1"},
         {"BEZEL_TEST_FILTER", "child_probe_under_missing_toolchain"},
         {kChildGuard, "1"},
         {"BEZEL_TEST_LIST", ""},
         {"BEZEL_TEST_ENUMERATE", ""},
         {"BEZEL_SHARD_COUNT", ""},
         {"BEZEL_SHARD_INDEX", ""},
         {"BEZEL_SHARD_SUMMARY", summaryPath}},
        &execErrno);
    const std::string sum = readAll(summaryPath);

    BEZEL_EXPECT_FALSE(timedOut);
    BEZEL_EXPECT_EQ(execErrno, 0);
    BEZEL_EXPECT_TRUE(status >= 0);

    // The shard JSON must carry the additive `checked_toolchain_skips`
    // field with a positive value. Pre-fix the field is absent (the entire
    // additive-field code path was added by #3733), so this is the FAIL-
    // WITHOUT vehicle for the source-half assertion that the shard
    // summary carries the count tests/run_parallel.sh sums.
    BEZEL_EXPECT_TRUE(sum.find("\"checked_toolchain_skips\":") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(sum.find("\"checked_toolchain_skips\":0") ==
                      std::string::npos);

    if (currentTestFailed()) {
        std::cout << "--- child output ---\n" << outPath << "\n---\n"
                  << readAll(outPath) << "\n--- shard json ---\n"
                  << sum << "\n--- end ---\n";
    }
}
