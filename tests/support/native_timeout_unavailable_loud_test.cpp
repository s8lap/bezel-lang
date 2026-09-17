// fix #3732 — a COMPILE / RUN TIMEOUT in the AOT harness must be LOUD and
// DISTINGUISHABLE from a compile reject.
//
// WHY THIS FILE EXISTS. `runAot` used to report a compile deadline expiry
// (`aotCompileTimeoutSec`, 120s default, fired inside the forked compile
// child) and a program-run deadline expiry (`aotTimeoutSec`, 30s default,
// fired when the compiled executable does not terminate in time) the SAME
// way it reports the compiler's own verdict: `ran = false` plus one
// `Severity::Error` carrying the message. Those are byte-for-byte the two
// shapes this tree's negative assertions test for —
//     BEZEL_EXPECT_TRUE(<var>.diagnostics.hasErrors());   (272 sites)
//     BEZEL_EXPECT_FALSE(<var>.ran);                      (205 sites)
// — so a real front-end hang or a runaway program would have turned ~477
// assertions GREEN while certifying nothing. (Measured 2026-09-03 by #3007
// over the 1,446 test files that call a runAot* helper — #3732's headline
// 1,092 does NOT reproduce, for the same reason #3007's does not: the
// headline counted all 3,015 hasErrors() assertions in tests/, most of
// which belong to the in-process JIT/sema harnesses this path cannot
// reach. Same defect, smaller blast radius.)
//
// Before #3732 this was latent only because harness timeouts are rare: a
// front-end that hangs forever is a compiler bug (filed), a clang link that
// exceeds 120s is a toolchain problem (rare on warm cache, rare-cold-cache
// on 16-shard + lane builds — see CLAUDE.md "## Build & test speed"), and a
// program-run timeout requires a Bezel program that genuinely runs forever
// (`while true do end;` is the canonical shape, and it shows up in
// negative-test bodies across tests/codegen, tests/ownership, and
// tests/sema). Now that #2633's ASan oracle runs the same harness at scale,
// the timeouts it previously hid behind #2633's "fail only under ASan"
// gating have started surfacing as the gate's own failures.
//
// WHAT IS PINNED HERE, in increasing order of end-to-endness:
//   1. the chokepoint — noteTimeoutFailure() counts and fails the current
//      test (no InfraExpectation analog: there is no synthetic
//      timeout-probe use case in the harness's own self-tests — #3626's
//      reasoning for InfraExpectation does not extend here);
//   2. the result SHAPE — markTimeout() leaves ran=false, exitCode=-1 and
//      the reason at Severity::NOTE, so `hasErrors()` is FALSE and the
//      272-site assertion cannot be satisfied by a timeout;
//   3. END TO END — a real `runAot` against a `while true do end;` program,
//      with BEZEL_AOT_TIMEOUT_SEC=1 forcing the program-run deadline to fire
//      in well under a second, in a real child process, driving a test
//      written in the legacy negative-assertion style, asserted through the
//      child's own [ FAIL ] accounting. THIS is the fail-without vehicle: on
//      the pre-#3732 harness the same child reports `[  OK  ]` and exits 0,
//      because the timeout diagnostic was Severity::Error and hasErrors()
//      was true.
//
// CHOICE OF WHICH DEADLINE TO FIRE (run, not compile). The brief mentions
// BEZEL_AOT_COMPILE_TIMEOUT_SEC=1; we use BEZEL_AOT_TIMEOUT_SEC=1 (the
// PROGRAM run deadline) instead, because a `while true do end;` Bezel
// program is a portable hang trigger — every Bezel compiler produces an
// infinite loop in the generated LLVM IR — whereas forcing a COMPILE timeout
// would require either a deliberately large program (fragile, machine-
// dependent) or an always-slow typechecker input (introduces a permanent
// compiler defect to test against a different defect). The channel logic is
// identical for compile and run timeouts (both route through markTimeout);
// the e2e shape is the same; only the knob we dial is different. See the
// commit body for the full rationale.

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

#include "test_harness.hpp"
#include "support/aot_support.hpp"

namespace {

namespace aot = bezel::test::aot_detail;

// This process's own executable — the child in the end-to-end test is another
// run of this same test binary, so no path has to be plumbed in. Same shape
// as tests/support/aot_infra_loud_3007_test.cpp::selfExe().
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

// A program the harness MUST compile successfully and then NEVER terminate
// (canonical `while true do end;` — observable across tests/codegen and
// tests/ownership as the standard "hangs forever" shape). Used by the
// end-to-end test below to force the program-run deadline (aotTimeoutSec,
// 30s default; forced to 1s in the child) to fire in well under a second.
constexpr const char* kInfiniteLoopProgram =
    "module M; class C public static method Main(): int "
    "  while true do end; "
    "  return 0; "
    "end Main; end C;";

// The probe that #3732's burn-down points at — a test body in the legacy
// negative-assertion style (BEZEL_EXPECT_TRUE(hasErrors())). Used to
// demonstrate that an INFINITE-LOOP program is NOT read as a "rejection" by
// such a body: the harness gave up, and the negative assertion MUST fail.
constexpr const char* kLegacyNegativeAssertionProgram =
    "module M; class C public static method Main(): int "
    "  return noSuchThing; "
    "end Main; end C;";

}  // namespace

// ---------------------------------------------------------------------------
// 1. The chokepoint counts, fails the current test, and is otherwise inert
//    outside a test body. Modelled on aot_infra_loud_3007's
//    chokepoint_counts_and_expectation_intercepts but without the
//    InfraExpectation interceptor — #3732 does not need one (no synthetic
//    timeout-probe use case).
// ---------------------------------------------------------------------------
BEZEL_TEST(native_timeout_unavailable_loud,
           chokepoint_counts_and_fails_the_current_test) {
    const std::uint64_t before =
        aot::timeoutFailureCount().load(std::memory_order_relaxed);
    aot::noteTimeoutFailure("probe", "synthetic compile hang (not a real outage)");
    BEZEL_EXPECT_TRUE(
        aot::timeoutFailureCount().load(std::memory_order_relaxed) == before + 1);
    // Belt: the channel tripped the current test. A bare
    // `BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors())` AFTER this point cannot
    // unset it — the same structural argument #3007 makes for
    // noteInfraFailure.
    BEZEL_EXPECT_TRUE(::bezel::test::currentTestFailed());
}

// ---------------------------------------------------------------------------
// 2. The result SHAPE: a TIMEOUT must NOT satisfy hasErrors().
//    This is the single assertion that decides whether the 272 sites are
//    protected; everything else in this file supports it.
// ---------------------------------------------------------------------------
BEZEL_TEST(native_timeout_unavailable_loud,
           timeout_result_does_not_satisfy_has_errors) {
    bezel::test::JitResult r;
    aot::markTimeout(r, "runAot", "synthetic program hang (not a real timeout)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, -1);
    // The load-bearing assertion of this whole change.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.diagnostics.errorCount(), static_cast<std::size_t>(0));
    // The reason is still RECORDED, at Note severity, so a human reading a
    // dump sees what happened — it is the severity, not the message, that the
    // negative assertions key on.
    BEZEL_EXPECT_EQ(r.diagnostics.diagnostics().size(),
                    static_cast<std::size_t>(1));
    if (r.diagnostics.diagnostics().size() == 1) {
        const auto& d = r.diagnostics.diagnostics().front();
        BEZEL_EXPECT_TRUE(d.severity == bezel::Severity::Note);
        BEZEL_EXPECT_TRUE(d.message.find("[TIMEOUT]") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// 3a. THE PROBE (the legacy-shape witness). Written in exactly the legacy
//     negative-assertion style. Under NORMAL conditions it is a genuine,
//     non-vacuous test: the program really is rejected. Under the program-
//     run deadline firing (BEZEL_AOT_TIMEOUT_SEC=1 + `while true do end;`)
//     it is the false green #3732 is about, and test 3b runs it in a child
//     process to observe that it no longer is one.
//     (Class A for scripts/check_vacuous_tests.py: `hasErrors()` in its
//     degenerate direct form is reject-only by definition.)
// ---------------------------------------------------------------------------
BEZEL_TEST(native_timeout_unavailable_loud,
           legacy_negative_assertion_shape) {
    auto r = bezel::test::runAot(kLegacyNegativeAssertionProgram);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// 3b. END TO END, and the FAIL-WITHOUT vehicle. Re-run THIS binary, filtered
//     to the probe in 3c, with BEZEL_AOT_TIMEOUT_SEC=1 forcing the program-
//     run deadline to fire in well under a second for a `while true do
//     end;` program — a real, deterministic timeout, no fault-injection
//     hook, uid-independent. (We use the program-run deadline, not the
//     compile deadline — see the file header's "CHOICE OF WHICH DEADLINE
//     TO FIRE" note for why.)
//
//     WITH the fix   : the child runs the 3c probe, the harness's timeout
//                      channel flips currentTestFailed() (noteTimeoutFailure)
//                      AND demotes the diagnostic to Severity::Note so the
//                      probe's BEZEL_EXPECT_TRUE(hasErrors()) assertion
//                      fails. The child reports [ FAIL ] for the probe,
//                      prints [ TIMEOUT ] to mark the timeout, exits 1.
//                      The atexit tally line reads
//                      `TIMEOUT: 1 compile/run timeout(s)` — the harness
//                      side signal that the channel was live in this
//                      process.
//     WITHOUT the fix: the same child runs the 3c probe, the harness
//                      reports the hang as Severity::Error with "program
//                      did not terminate" — `hasErrors()` is TRUE, the
//                      legacy-shape assertion PASSES, the test reports
//                      [ OK ], the child exits 0. No TIMEOUT line is
//                      observable. That is the false green #3732 is about.
//
//     The 3a legacy-shape probe is included in the same suite (it does not
//     need the e2e shape — it just exercises the shape in-process against
//     a real rejection, confirming the legacy assertion is genuine when
//     the harness is healthy). 3a is filtered out of the child by the
//     BEZEL_TEST_FILTER in the spawn env.
//
//     Every BEZEL_* variable the child must not inherit is neutralised by
//     setting it EMPTY (test_main.cpp treats an empty value as unset for
//     all of them), because the spawn helper can setenv but not unsetenv.
// ---------------------------------------------------------------------------
BEZEL_TEST(native_timeout_unavailable_loud,
           run_timeout_fails_a_legacy_shape_probe_loudly_3732) {
    // Belt and braces against recursion: the filter below cannot match this
    // test's name, and this guard stops it even if that ever changes.
    if (const char* c = std::getenv("BEZEL_3732_E2E_CHILD");
        c && *c) return;

    const std::string exe = selfExe();
    BEZEL_EXPECT_FALSE(exe.empty());
    if (exe.empty()) return;

    const std::filesystem::path dir = wtScratchDir("timeout-3732-e2e");
    const std::string outPath = (dir / "child.out").string();

    bool timedOut = false;
    int execErrno = 0;
    const int status = aot::spawnCaptured(
        {exe}, outPath, outPath, /*timeoutSec=*/300, timedOut,
        {{"BEZEL_AOT_TIMEOUT_SEC", "1"},
         {"BEZEL_TEST_FILTER", "infinite_loop_under_short_deadline_fails_3732"},
         {"BEZEL_3732_E2E_CHILD", "1"},
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
    // (CLAUDE.md "Lanes §2"). Pin the RUN + the [ FAIL ].
    BEZEL_EXPECT_TRUE(
        out.find("[ RUN  ] native_timeout_unavailable_loud."
                 "infinite_loop_under_short_deadline_fails_3732") !=
        std::string::npos);
    BEZEL_EXPECT_TRUE(
        out.find("1 passed, 0 failed") == std::string::npos);

    // The three faces of loud: per-test verdict, marker, process exit.
    BEZEL_EXPECT_TRUE(
        out.find("[ FAIL ] native_timeout_unavailable_loud."
                 "infinite_loop_under_short_deadline_fails_3732") !=
        std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("[ TIMEOUT ]") != std::string::npos);
    // The atexit tally (`TIMEOUT: N compile/run timeout(s)`) is the harness-
    // side signal: 0 is positive confirmation the channel is wired.
    BEZEL_EXPECT_TRUE(
        out.find("TIMEOUT: 1 compile/run timeout(s)") != std::string::npos);
    // And a nonzero process exit, so a runner that only reads rc still sees it.
    BEZEL_EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) != 0);

    if (::bezel::test::currentTestFailed())
        std::cout << "--- child output ---\n" << out << "\n--- end ---\n";
}

// ---------------------------------------------------------------------------
// 3c. THE FAIL-WITHOUT PROBE that runs INSIDE the child above. A genuine
//     infinite-loop Bezel program, run under a 1-second program deadline,
//     in a test body that ASSERTS the legacy shape — exactly the shape the
//     ~477 vulnerable sites across tests/ use (3a demonstrates it for the
//     REJECTION case, this one is the TIMEOUT twin). Pre-fix the harness
//     surfaces the hang as Severity::Error with the "program did not
//     terminate" message, `hasErrors()` is TRUE, and the assertion passes:
//     the test reports `[ OK ]` and the child exits 0 — the false green
//     #3732 is about. Post-fix the harness trips currentTestFailed() via
//     noteTimeoutFailure() AND demotes the diagnostic to Severity::Note so
//     `hasErrors()` is FALSE; the assertion fails AND the harness trips the
//     test: `[ FAIL ]` and exit 1. Run only when BEZEL_3732_E2E_CHILD=1
//     (the parent-side probe above sets that env var on the child).
// ---------------------------------------------------------------------------
BEZEL_TEST(native_timeout_unavailable_loud,
           infinite_loop_under_short_deadline_fails_3732) {
    if (!std::getenv("BEZEL_3732_E2E_CHILD")) return;
    auto r = bezel::test::runAot(kInfiniteLoopProgram);
    BEZEL_EXPECT_FALSE(r.ran);
    // Legacy-shape assertion: pre-fix TRUE (Severity::Error), post-fix FALSE
    // (Severity::Note). The post-fix flip is what makes this the FAIL-WITHOUT
    // pin.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
