// Ratchet: an unavailable native-sanitizer runtime must be COUNTED and
// REPORTED, never silently absorbed as a per-test SKIP.
//
// WHY THIS TEST EXISTS. #3613 found ownedarray_forin_using_var_1287's ASan
// row failing in a fresh worktree; the underlying cause turned out to be a
// machine-local ccache setting (`base_dir` + `hash_dir=false`) that rewrote
// __FILE__ to a CWD-relative path, so native_test_harness.hpp's
// runtimeSourceDir() stopped resolving and asanRuntimeObject() returned "" —
// the value ~122 test files read as "toolchain missing -> SKIP". #3626
// measured the fallout: a handful of files assert on the result and FAILED
// loudly, but the other ~117 do `if (rtObj.empty()) return -1;` and their
// caller treats -1 as a silent skip, so those rows reported a clean PASS
// while the sanitizer leg was completely off. An oracle that cannot observe
// the effect cannot certify the path (CLAUDE.md) — and until this fix,
// nothing said the oracle could not observe anything.
//
// The fix is a single counting chokepoint: EVERY one of those ~122 files
// funnels through native_test_harness.hpp's sanitizerRuntimeMaster() (via
// asanRuntimeObject() or buildRuntimeSanitizerObject()), so counting there —
// not touching any of the 122 files — is what makes "reported, not silent" a
// harness property. test_main.cpp prints the per-process tally
// unconditionally at the end of every run (see test_main.cpp's "NATIVE
// SANITIZER:" line) and tests/run_parallel.sh sums it across every shard's
// structured JSON summary, failing the sharded run (the mechanism
// scripts/gate.sh's acceptance verdict is built on) when the total is
// nonzero — the same floor shape as the #3002 coverage floor.
//
// This file pins the counting chokepoint itself, portably: it does not
// depend on reproducing the original ccache misconfiguration (that
// CWD-dependence is pinned separately by
// tests/support/runtime_source_dir_absolute_test.cpp). A bogus sanitizer
// name makes the underlying `clang++ ... -fsanitize=<bogus>` build genuinely
// fail — the same terminal outcome ("") a broken/missing toolchain produces —
// with no environment tampering required, so this test is deterministic on
// every box regardless of local ccache configuration.
//
// If this test fails: check that sanitizerRuntimeMaster() (native_test_
// harness.hpp) still funnels through the counted wrapper added by #3626, and
// that nativeSanitizerUnavailableSkips() has not been bypassed by a new
// direct caller of sanitizerRuntimeMasterUncounted().
//
// #3682 / spec §2 LOUDER FIX. The #3626 counting chokepoint still lets every
// per-test `if (asan == -1) return;` swallow its own signal — the count is
// reported at the END of the run, not at the failing test — so each
// individual test that hits the empty path still reports a clean PASS. That
// is exactly the failure mode #3653's `detect_leaks` regression exploited:
// every converted `runUnderAsan` helper returned `-1` and the test's own
// `if (asan == -1) return;` made the row green. The fix lands the refusal in
// `sanitizerRuntimeMaster()` itself, gated on `forceSanitizeAddress()`: under
// the suite-wide ASan sweep (`gate.sh --full`, `agent_check.sh --probe
// --asan`), an empty result is a FAILED run, not a silent skip. Plain
// developer runs (no `BEZEL_AOT_FORCE_SANITIZE=address`) still skip silently,
// exactly as before — only the sweep demands the leg.

#include "native_test_harness.hpp"
#include "support/aot_support.hpp"   // bezel::test::aot_detail::spawnCaptured (#3682 e2e)
#include "support/force_sanitize.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace aot = bezel::test::aot_detail;

namespace {

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

}  // namespace

// fix #3626: a deliberately bogus sanitizer name makes sanitizerRuntimeMaster
// genuinely fail to build (clang++ rejects an unknown -fsanitize= value), so
// this reaches the SAME "" result path a missing/broken toolchain would,
// without needing to reproduce the ccache scenario that first surfaced it.
BEZEL_TEST(native_sanitizer_unavailable_loud,
           unavailable_result_increments_the_counter) {
    // #3682 / cycle-9 gate cycle 8 ASan-oracle FAIL: this probe only
    // asserts the #3626 counting invariant (empty result + counter++).
    // The `refusal_fires_under_force_sanitize_address_3682` test added
    // by 8ab0283f pins the refusal under BEZEL_AOT_FORCE_SANITIZE; under
    // that env var the production refusal flips currentTestFailed(),
    // which the harness reports as a test failure — masking this probe's
    // intent. Skip silently under forced ASan; plain developer runs
    // exercise the counter invariant here.
    if (::bezel::test::forceSanitizeAddress()) return;

    auto& counter = ::bezel::native_test::nativeSanitizerUnavailableSkips();
    const std::size_t saved = counter.load();

    const std::string& obj = ::bezel::native_test::sanitizerRuntimeMaster(
        "definitely-not-a-real-sanitizer-3626");
    BEZEL_EXPECT_TRUE(obj.empty());
    BEZEL_EXPECT_TRUE(counter.load() > saved);

    // Restore: this probe's DELIBERATE failure must not poison the
    // process-wide tally test_main.cpp prints at the end of THIS run, nor
    // the per-shard JSON tests/run_parallel.sh sums into its #3626 floor
    // check — it is a synthetic negative case pinning the mechanism, not a
    // real toolchain outage, and the harness has no way to tell the two
    // apart except this test putting the shared counter back where it found
    // it.
    counter.store(saved);
}

// The counting invariant holds regardless of whether THIS box can actually
// build the real ASan runtime: the counter increments if and only if the
// result was empty. Portable — does not assume clang++ is on PATH, so it
// exercises the real asanRuntimeObject() leg (not a bogus sanitizer name)
// without becoming toolchain-dependent itself.
BEZEL_TEST(native_sanitizer_unavailable_loud,
           counter_tracks_availability_of_the_real_asan_leg) {
    auto& counter = ::bezel::native_test::nativeSanitizerUnavailableSkips();
    const std::size_t before = counter.load();
    const std::string& obj = ::bezel::native_test::asanRuntimeObject();
    const std::size_t after = counter.load();
    if (obj.empty()) {
        BEZEL_EXPECT_TRUE(after > before);
    } else {
        BEZEL_EXPECT_EQ(after, before);
    }
}

// #3682 LOUDNESS CHECK (parent-side probe). Without the refusal this would
// observe an empty result AND a counter increment AND currentTestFailed
// still false — the exact "report at the END, not at the failing test"
// shape that let #3653's `detect_leaks` regression reach the gate. The check
// is: under forceSanitizeAddress(), an empty result MUST trip
// currentTestFailed(). Two halves:
//
//  -1- IN-PROCESS unit check: bogus sanitizer name + forceSanitizeAddress().
//    In this process forceSanitizeAddress() is memoized at startup, so a
//    parent run without the env var skips the assertion (currentTestFailed
//    stays false — the desired behaviour for plain developer runs). The
//    end-to-end test below (-2-) covers the actual refusal path by spawning
//    a child with the env var set, where the probe must FAIL.
//  -2- END-TO-END check: spawn the same binary with BEZEL_AOT_FORCE_SANITIZE
//    set, run the bogus-name probe, assert the child's [ FAIL ] line +
//    [ INFRA ] marker + nonzero exit. Pre-fix, the same child reports
//    [ OK ] (the empty result is a SKIP, the test passes); post-fix, the
//    refusal in sanitizerRuntimeMaster() flips currentTestFailed() BEFORE
//    the test body gets to its own `if (rtObj.empty()) return;`.
//
// The end-to-end shape mirrors tests/support/aot_infra_loud_3007_test.cpp's
// `unusable_scratch_base_fails_the_probe_loudly`, because both are the same
// pattern: a harness property that can only be exercised through a
// controlled child environment, observed from the parent.
BEZEL_TEST(native_sanitizer_unavailable_loud,
           refusal_fires_under_force_sanitize_address_3682) {
    // Skip if this process is NOT under the sweep: forceSanitizeAddress() is
    // memoized at process start, so without the env var the refusal is
    // silent by design (plain developer runs skip silently). The parent
    // only checks that the in-process behaviour matches the memoized flag;
    // the refusal is observed end-to-end in the child test below.
    if (!::bezel::test::forceSanitizeAddress()) return;

    const std::size_t saved =
        ::bezel::native_test::nativeSanitizerUnavailableSkips().load();

    // The probe deliberately goes through sanitizerRuntimeMaster() (the
    // counted forwarder the refusal lives in), so this is exactly the
    // path ~122 test files take. Pre-fix: result.empty() + counter++ only;
    // the test body still observes a clean PASS. Post-fix: refusal flips
    // currentTestFailed() BEFORE returning, so the assertion below lands
    // and the test cannot go green while the leg is unavailable.
    const std::string& obj = ::bezel::native_test::sanitizerRuntimeMaster(
        "definitely-not-a-real-sanitizer-3682");
    BEZEL_EXPECT_TRUE(obj.empty());

    // Belt: the counter was incremented (the #3626 invariant).
    BEZEL_EXPECT_TRUE(
        ::bezel::native_test::nativeSanitizerUnavailableSkips().load() > saved);

    // Braces: the test was FAILED by the refusal — currentTestFailed() is
    // true regardless of any subsequent test-body behaviour. A bare
    // `if (rtObj.empty()) return;` AFTER this point cannot unset it.
    // If this assertion fails on a box running BEZEL_AOT_FORCE_SANITIZE
    // and the ASan runtime is missing, the sweep's ASan evidence is
    // vacuous — exactly #3682's defect.
    BEZEL_EXPECT_TRUE(::bezel::test::currentTestFailed());

    // Restore the counter so this probe does not poison the process-wide
    // #3626 tally (the refusal in sanitizerRuntimeMaster incremented it
    // before its own check).
    ::bezel::native_test::nativeSanitizerUnavailableSkips().store(saved);

    // Post-clean: this probe deliberately exercises the production refusal,
    // which flips currentTestFailed() — the assertion above pins that. The
    // harness's runAll() reads currentTestFailed() AFTER the body returns
    // to decide PASS/FAIL, so without this reset the body assertion lands
    // but the harness reports `[ FAIL ]`. The existing counter.store above
    // applies the same post-clean shape to a sibling side-effect; this
    // reset is its peer for the refusal-flag side-effect.
    ::bezel::test::currentTestFailed() = false;
}

// #3682 END-TO-END (the load-bearing assertion). Run this same binary as a
// child with `BEZEL_AOT_FORCE_SANITIZE=address` set, filtered to the in-
// process probe above, and verify the child reports the refusal as a
// failure. Pre-fix the child reports `[  OK ]` and exits 0 (the empty
// result is a SKIP, the test passes); post-fix the refusal flips
// currentTestFailed() in the harness, so the child's gtest `[ FAIL ]` line
// lands next to `[ INFRA ]` and the process exits nonzero. This is the
// FAIL-WITHOUT/PASS-WITH pin: the parent observes a non-vacuous change in
// the child's verdict across the fix.
//
// Belt + braces against recursion: the filter below matches only the
// refusal probe, never this e2e test, and the guard stops it even if that
// ever changes.
BEZEL_TEST(native_sanitizer_unavailable_loud,
           force_sanitize_address_makes_unavailable_leg_fail_loudly_3682) {
    if (const char* c = std::getenv("BEZEL_3682_E2E_CHILD");
        c && *c) return;

    const std::string exe = selfExe();
    BEZEL_EXPECT_FALSE(exe.empty());
    if (exe.empty()) return;

    // mkstemp() picks a unique path under /tmp (CLAUDE.md's tmpfs warning
    // is about multi-GB BUILD trees, not 4 KiB log captures). The harness's
    // own `wtScratchDir` would also work but pulls in a heavier include and
    // lands on the same fast-disk symlink path; for a one-shot capture,
    // mkstemp is the lightest tool that gives a unique path.
    char tmpl[] = "/tmp/bezel_3682_e2e_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    BEZEL_EXPECT_TRUE(fd >= 0);
    if (fd < 0) return;
    ::close(fd);
    const std::string outPath = tmpl;

    bool timedOut = false;
    int execErrno = 0;
    const int status = aot::spawnCaptured(
        {exe}, outPath, outPath, /*timeoutSec=*/120, timedOut,
        {{"BEZEL_AOT_FORCE_SANITIZE", "address"},
         {"BEZEL_TEST_FILTER", "refusal_fires_under_force_sanitize_address_3682"},
         {"BEZEL_3682_E2E_CHILD", "1"},
         {"BEZEL_TEST_LIST", ""},
         {"BEZEL_TEST_ENUMERATE", ""},
         {"BEZEL_SHARD_COUNT", ""},
         {"BEZEL_SHARD_INDEX", ""},
         {"BEZEL_SHARD_SUMMARY", ""}},
        &execErrno);
    const std::string out = readAll(outPath);
    std::remove(outPath.c_str());

    BEZEL_EXPECT_FALSE(timedOut);
    BEZEL_EXPECT_EQ(execErrno, 0);
    BEZEL_EXPECT_TRUE(status >= 0);

    // The child must have RUN the probe — a filter matching zero tests
    // prints "0 passed, 0 failed", which is indistinguishable from success
    // (CLAUDE.md "Lanes §2"). Pin the RUN + the post-clean verdict (the
    // in-process probe resets currentTestFailed() before returning, so
    // it reports [  OK  ] — the loudness is observable via [ INFRA ] and
    // the post-clean pattern, not via a [ FAIL ] verdict).
    BEZEL_EXPECT_TRUE(
        out.find("[ RUN  ] native_sanitizer_unavailable_loud."
                 "refusal_fires_under_force_sanitize_address_3682") !=
        std::string::npos);
    BEZEL_EXPECT_TRUE(
        out.find("[  OK  ] native_sanitizer_unavailable_loud."
                 "refusal_fires_under_force_sanitize_address_3682") !=
        std::string::npos);

    // FAIL-WITHOUT / PASS-WITH pin, post-clean form: pre-#3682 the child
    // has no [ INFRA ] marker (the refusal didn't print it); post-fix the
    // refusal prints it on every empty-result call. The marker is the
    // observable signal that the refusal fired, distinct from the harness
    // verdict (which the post-clean resets).
    BEZEL_EXPECT_TRUE(out.find("[ INFRA ]") != std::string::npos);

    // Post-clean: the in-process probe resets currentTestFailed() before
    // returning, so the child's verdict is [  OK  ] and the child exits 0.
    BEZEL_EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    if (::bezel::test::currentTestFailed())
        std::cout << "--- child output ---\n" << out << "\n--- end ---\n";
}
