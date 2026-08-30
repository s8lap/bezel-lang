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

#include "native_test_harness.hpp"
#include "test_harness.hpp"

// fix #3626: a deliberately bogus sanitizer name makes sanitizerRuntimeMaster
// genuinely fail to build (clang++ rejects an unknown -fsanitize= value), so
// this reaches the SAME "" result path a missing/broken toolchain would,
// without needing to reproduce the ccache scenario that first surfaced it.
BEZEL_TEST(native_sanitizer_unavailable_loud,
           unavailable_result_increments_the_counter) {
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
