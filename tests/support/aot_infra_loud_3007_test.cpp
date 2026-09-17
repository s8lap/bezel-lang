// fix #3007 — an INFRASTRUCTURE failure in the AOT harness must be LOUD and
// DISTINGUISHABLE from a compiler rejection.
//
// WHY THIS FILE EXISTS. `runAot` used to report a failed workdir create, a
// failed source write, a refused fork and a failed exec the same way it reports
// the compiler's own verdict: `ran = false` plus one `Severity::Error` carrying
// the message. Those are byte-for-byte the two shapes this tree's negative
// assertions test for —
//     BEZEL_EXPECT_TRUE(<var>.diagnostics.hasErrors());   (272 sites)
//     BEZEL_EXPECT_FALSE(<var>.ran);                      (205 sites)
// — so a full disk turned ~477 assertions GREEN while certifying nothing.
// (Measured 2026-09-03 over the 1,446 test files that call a runAot* helper.
// #3007 states 1,092; that does not reproduce — it counted all 3,015
// hasErrors() assertions in tests/, most of them on harnesses this cannot
// reach. Same defect, smaller blast radius.) That
// was latent only because the harness workdir sat on a 1.3 TiB spindle; since
// scripts/lane_setup.sh symlinks scratchBase() onto /mnt/fast (which HAS hit
// ENOSPC, #3003) it is armed. `hasErrors()` cannot tell "the rule under test
// rejected this" from "anything at all went wrong" (#2988), and an oracle that
// cannot observe the effect cannot certify the path.
//
// WHAT IS PINNED HERE, in increasing order of end-to-endness:
//   1. the chokepoint — noteInfraFailure() counts, and an InfraExpectation
//      intercepts it without failing the current test (the mechanism the
//      harness's own probes need, and the reason a synthetic probe cannot
//      poison the process-wide `INFRA:` tally);
//   2. the result SHAPE — markInfra() leaves ran=false, exitCode=-1 and the
//      reason at Severity::NOTE, so `hasErrors()` is FALSE and the 272-site
//      assertion cannot be satisfied by an infra failure;
//   3. the two per-stage causes that carry an errno (writeSource);
//   4. END TO END — a real `runAot` call against an unusable scratch base, in a
//      real child process, driving a test written in exactly the legacy
//      negative-assertion style, asserted through the child's own [ FAIL ]
//      accounting. THIS is the fail-without vehicle: on the pre-#3007 harness
//      the same child reports `[  OK  ]` and exits 0.

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

// A program the front end must REJECT (`noSuchThing` is not declared). One
// token away from the known-good program in
// tests/codegen/aot_checked_lenient_2214_test.cpp, so a rejection here is the
// name resolution and nothing else.
constexpr const char* kRejectedProgram =
    "module M; class C public static method Main(): int "
    "  return noSuchThing; "
    "end Main; end C;";

// This process's own executable — the child in the end-to-end test is another
// run of this same test binary, so no path has to be plumbed in.
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
// 1. The chokepoint counts, and an InfraExpectation intercepts it.
// ---------------------------------------------------------------------------
BEZEL_TEST(aot_infra_loud_3007, chokepoint_counts_and_expectation_intercepts) {
    const std::uint64_t before =
        aot::infraFailureCount().load(std::memory_order_relaxed);
    {
        aot::InfraExpectation ex;
        aot::noteInfraFailure("probe", "synthetic ENOSPC (not a real outage)");
        BEZEL_EXPECT_EQ(ex.count, static_cast<std::uint64_t>(1));
        BEZEL_EXPECT_TRUE(ex.lastWhere == "probe");
        BEZEL_EXPECT_TRUE(ex.lastReason.find("synthetic ENOSPC") !=
                          std::string::npos);
        // Counted while in scope...
        BEZEL_EXPECT_EQ(aot::infraFailureCount().load(std::memory_order_relaxed),
                        before + 1);
        // ...and, crucially, the interception did NOT fail this test. If it
        // had, this file could never assert anything about the mechanism.
        BEZEL_EXPECT_FALSE(::bezel::test::currentTestFailed());
    }
    // ...and withdrawn on scope exit, so the process-wide `INFRA: N failure(s)`
    // line stays true. A deliberate probe is not an outage.
    BEZEL_EXPECT_EQ(aot::infraFailureCount().load(std::memory_order_relaxed),
                    before);
}

// ---------------------------------------------------------------------------
// 2. The result SHAPE: an infra failure must NOT satisfy hasErrors().
//    This is the single assertion that decides whether the 272 sites are
//    protected; everything else in this file supports it.
// ---------------------------------------------------------------------------
BEZEL_TEST(aot_infra_loud_3007, infra_result_does_not_satisfy_has_errors) {
    bezel::test::JitResult r;
    {
        aot::InfraExpectation ex;
        aot::markInfra(r, "runAot", "could not create a work dir (synthetic)");
        BEZEL_EXPECT_EQ(ex.count, static_cast<std::uint64_t>(1));
    }
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
        BEZEL_EXPECT_TRUE(d.message.find("[INFRA]") != std::string::npos);
        BEZEL_EXPECT_TRUE(d.message.find("work dir") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// 3. writeSource reports the CAUSE, not merely "false". "No space left on
//    device" is what makes the [ INFRA ] marker actionable.
// ---------------------------------------------------------------------------
BEZEL_TEST(aot_infra_loud_3007, write_source_failure_carries_the_errno) {
    const std::filesystem::path dir = wtScratchDir("infra-3007-write");
    // A regular FILE where a directory would have to be: the write then fails
    // with ENOTDIR for ANY uid, so this probe is deterministic even under root
    // (a mode-0500 directory would not be).
    const std::filesystem::path blocker = dir / "blocker";
    { std::ofstream(blocker.string(), std::ios::binary) << "x"; }
    const std::string target = (blocker / "prog.bzl").string();

    std::string why;
    const bool ok = aot::writeSource(target, "module M;", &why);
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_FALSE(why.empty());
    BEZEL_EXPECT_TRUE(why.find("failed to write the program source") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(why.find(target) != std::string::npos);

    // The happy path is unchanged: same helper, writable target, no reason.
    std::string why2 = "poison";
    const bool ok2 = aot::writeSource((dir / "ok.bzl").string(), "module M;",
                                      &why2);
    BEZEL_EXPECT_TRUE(ok2);
    BEZEL_EXPECT_TRUE(why2.empty());
}

// ---------------------------------------------------------------------------
// 4a. THE PROBE. Written in exactly the legacy negative-assertion style — the
//     shape of ~477 assertions in this tree. Under normal conditions it is a
//     genuine, non-vacuous test: the program really is rejected. Under an
//     unusable scratch base it is the false green #3007 is about, and test 4b
//     runs it in a child process to observe that it no longer is one.
//     (Class A for scripts/check_vacuous_tests.py: `hasErrors()` in its
//     degenerate direct form is reject-only by definition.)
// ---------------------------------------------------------------------------
// AMBIGUOUS-RAN-FALSE-OK: this row is deliberately preserved as the FAIL-WITHOUT vehicle for issue #3007's infra-loud fix (see test 4b below). The detector's per-test suppression marker ports #3007 into the ratchet's "not a hole" set without losing the demonstration.
BEZEL_TEST(aot_infra_loud_3007, legacy_negative_assertion_shape) {
    auto r = bezel::test::runAot(kRejectedProgram);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// 4b. END TO END, and the FAIL-WITHOUT vehicle. Re-run THIS binary, filtered to
//     the probe above, with BEZEL_AOT_SCRATCH_DIR pointing inside a regular
//     file so makeWorkDir() genuinely fails — a real ENOSPC-class outage, no
//     fault injection hook, uid-independent.
//
//     WITH the fix   : the child reports [ FAIL ], prints [ INFRA ], exits 1,
//                      and its exit tally reads `INFRA: 1 failure(s)`.
//     WITHOUT the fix: the identical child reports [  OK  ] and exits 0,
//                      because the infra error is an Error and hasErrors() is
//                      true — which is precisely the defect.
//
//     Every BEZEL_* variable the child must not inherit is neutralised by
//     setting it EMPTY (test_main.cpp treats an empty value as unset for all of
//     them), because the spawn helper can setenv but not unsetenv.
// ---------------------------------------------------------------------------
BEZEL_TEST(aot_infra_loud_3007, unusable_scratch_base_fails_the_probe_loudly) {
    // Belt and braces against recursion: the filter below cannot match this
    // test's name, and this guard stops it even if that ever changes.
    if (const char* c = std::getenv("BEZEL_AOT_INFRA_SELFTEST_CHILD");
        c && *c) return;

    const std::string exe = selfExe();
    BEZEL_EXPECT_FALSE(exe.empty());
    if (exe.empty()) return;

    const std::filesystem::path dir = wtScratchDir("infra-3007-e2e");
    const std::filesystem::path blocker = dir / "not-a-directory";
    { std::ofstream(blocker.string(), std::ios::binary) << "x"; }
    const std::string badBase = (blocker / "base").string();
    const std::string outPath = (dir / "child.out").string();

    bool timedOut = false;
    int execErrno = 0;
    const int status = aot::spawnCaptured(
        {exe}, outPath, outPath, /*timeoutSec=*/300, timedOut,
        {{"BEZEL_AOT_SCRATCH_DIR", badBase},
         {"BEZEL_TEST_FILTER", "legacy_negative_assertion_shape"},
         {"BEZEL_AOT_INFRA_SELFTEST_CHILD", "1"},
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

    // The child must have RUN the probe — a filter matching zero tests prints
    // "0 passed, 0 failed", which is indistinguishable from success (LANE_RULES
    // §2), so pin the match count before believing anything else.
    BEZEL_EXPECT_TRUE(out.find("[ RUN  ] aot_infra_loud_3007."
                               "legacy_negative_assertion_shape") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("1 passed, 0 failed") == std::string::npos);

    // The three faces of loud: the per-test verdict, the marker, the tally.
    BEZEL_EXPECT_TRUE(out.find("[ FAIL ] aot_infra_loud_3007."
                               "legacy_negative_assertion_shape") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("[ INFRA ]") != std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("INFRA: 1 failure(s)") != std::string::npos);
    // And a nonzero process exit, so a runner that only reads rc still sees it.
    BEZEL_EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) != 0);

    if (::bezel::test::currentTestFailed())
        std::cout << "--- child output ---\n" << out << "\n--- end ---\n";
}
