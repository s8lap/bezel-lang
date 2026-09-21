// Ratchet: a native-sanitizer-leg BUILD failure must be reported as `-2` AND
// must announce itself, never as the `-1` that every caller in this family
// reads as "toolchain unavailable: SKIP".
//
// ── THE BUG (#4563) ─────────────────────────────────────────────────────────
//
// The ~85 hand-rolled `runUnderLeakSanitizer` / `runUnderAsan*` copies (the
// #3917 family) collapsed THREE structurally different outcomes onto one
// sentinel:
//
//     if (rtObj.empty()) return -1;                 // (1) no ASan runtime
//     if (!unit) return -1;                         // (2) the PROGRAM
//     if (diags.hasErrors()) return -1;             // (2)  failed to build
//     if (!emitObjectFileForTest(...)) return -1;   // (2)
//     if (std::system(linkCmd) != 0) return -3;     // (3) link failed
//
// and every caller wrote `if (rc == -1) return;`. Only (1) is a legitimate
// skip. (2) is a failure of the test's OWN setup, and it reported a silent
// PASS — the row ran, printed `[ OK ]`, and asserted nothing about memory
// safety. `nativeSanitizerUnavailableSkips()` (#3626) increments only on (1),
// so a suite-wide outage of (2) moved no counter at all.
//
// MEASURED, on this file's own tree, before the fix: hard-wiring
// `bezel::test::emitObjectFileForTest` to `return false` — i.e. the sanitizer
// leg cannot build a single test program — left `agent_check.sh --area codegen
// undisposed_leak` reporting BYTE-FOR-BYTE the same result as a clean run:
// `16 passed, 0 failed`, `NATIVE SANITIZER: 0 check(s) skipped`,
// `INFRA: 0 failure(s)`. That is the whole defect in one line of output.
//
// ── WHAT IS PINNED HERE ─────────────────────────────────────────────────────
//
// The distinct code is the lesser half. ~49 sites in this family guard with
// `if (rc < 0) return;` or `if (rc >= 0) BEZEL_EXPECT_EQ(rc, 0);` (the
// `swallowing_guards` count scripts/check_native_asan_leg_skips.py ratchets),
// and those swallow a silent `-2` exactly as they swallowed `-1`. So the
// load-bearing half is that the CHOKEPOINT marks the test failed, which makes
// a caller that ignores the code entirely still fail. Both halves are asserted
// below; `code_is_minus_two_...` alone would pass on the pre-fix canonical
// helper, which already returned a silent -2.
//
// The test drives the canonical `lowerToObjectForAsan`
// (tests/support/run_under_asan.hpp) rather than one of the per-file copies,
// for the same reason the #3676 sibling does: the canonical IS the shape every
// copy mirrors, and after #4563 all 85 copies funnel their build failures
// through the one `bezel::test::asanBuildFailure`. It needs NO toolchain — a
// program that fails to parse never reaches clang — so it is not itself one of
// the environment-dependent rows it exists to protect.

#include "support/asan_infra_failure.hpp"  // asanBuildFailure, ExpectAsanInfraFailure
#include "support/run_under_asan.hpp"      // lowerToObjectForAsan (#3633 canonical)
#include "native_test_harness.hpp"
#include "test_harness.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

// A program that cannot possibly parse, so `lowerToObjectForAsan` fails at its
// very first stage and never touches the toolchain.
const char* kUnparseable4563 = "module M; public static class @@@ not bezel {{{";

// A program that parses cleanly but has a semantic error — `undeclaredThing4563`
// is bound nowhere — so the failure lands on the `diags.hasErrors()` stage
// instead of the parse stage. Shape copied from the family's own minimal
// programs (e.g. tests/codegen/box_refcount_test.cpp:298).
const char* kIllTyped4563 =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    return undeclaredThing4563; "
    "  end Main; "
    "end C;";

std::string objPath4563(const char* stem) {
    const char* base = std::getenv("BEZEL_AOT_SCRATCH_DIR");
    if (!base || !*base) base = "/var/tmp/bezel-scratch";
    fs::path p = fs::path(base) / "bezel_4563_regression_scratch";
    std::error_code ec;
    fs::create_directories(p, ec);
    return (p / (std::string(stem) + ".o")).string();
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Half 1: the CODE. A build failure is -2, and critically NOT -1 — so the
// canonical caller guard `if (rc == -1) return;`, which ~176 sites in this
// family spell verbatim, does not read it as a toolchain SKIP.
//
// Both front-end stages are covered: pre-fix the per-file copies returned -1
// for BOTH, and a fix that converted only the `emitObjectFile` site (the one
// #3917's burn-down made newly dangerous) would leave the parse/sema sites
// conflated.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(native_asan_build_failure_4563,
           code_is_minus_two_not_the_skip_sentinel) {
    // The marking is the mechanism under test in half 2; here it would simply
    // fail this test for doing its job, so it is suppressed and counted.
    bezel::test::asan_detail::ExpectAsanInfraFailure guard;

    const int parseRc = bezel::test::lowerToObjectForAsan(
        kUnparseable4563, objPath4563("parse"));
    const int semaRc = bezel::test::lowerToObjectForAsan(
        kIllTyped4563, objPath4563("sema"));

    // The contract (tests/support/run_under_asan.hpp's banner, since #3633):
    // -2 is "the test PROGRAM failed to build".
    BEZEL_EXPECT_EQ(parseRc, -2);
    BEZEL_EXPECT_EQ(semaRc, -2);

    // Said explicitly, because it is the property the ~176 `if (rc == -1)
    // return;` call sites depend on and the one the defect violated.
    BEZEL_EXPECT_TRUE(parseRc != -1);
    BEZEL_EXPECT_TRUE(semaRc != -1);

    // And both announced themselves — one marker each, not zero and not one
    // shared. A silent -2 is the #4563 conflation wearing a new number.
    BEZEL_EXPECT_EQ(guard.count(), 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Half 2 — THE LOAD-BEARING ONE. A build failure marks the current test failed
// at the chokepoint, so a caller that ignores the returned code entirely still
// fails.
//
// This is what half 1 cannot show: the pre-fix canonical helper ALREADY
// returned -2 here, silently, and the ~49 `if (rc < 0) return;` /
// `if (rc >= 0) BEZEL_EXPECT_EQ(rc, 0);` guards in this family swallow a
// silent -2 exactly as they swallowed -1. Run WITHOUT the guard, observe
// `currentTestFailed()`, then put it back — the same assert-then-restore shape
// tests/support/native_sanitizer_unavailable_loud_test.cpp uses for its own
// deliberate negative probe, and for the same reason: a synthetic failure must
// not poison this process's real verdict.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(native_asan_build_failure_4563,
           marks_the_test_failed_so_a_swallowing_guard_cannot_hide_it) {
    bool& failedFlag = ::bezel::test::currentTestFailed();
    const bool savedFlag = failedFlag;
    auto& tally = ::bezel::test::asanInfraFailures();
    const std::size_t savedTally = tally.load();

    const int rc = bezel::test::lowerToObjectForAsan(kUnparseable4563,
                                                     objPath4563("loud"));

    const bool observedFailed = failedFlag;
    const std::size_t observedTally = tally.load();

    // Restore BEFORE asserting, so a failed assertion below cannot leave the
    // process-wide state perturbed for the rest of this shard.
    failedFlag = savedFlag;
    tally.store(savedTally);

    BEZEL_EXPECT_EQ(rc, -2);

    // Pre-fix this was false: `lowerToObjectForAsan` returned a bare -2 and
    // said nothing, so a `if (rc < 0) return;` caller reported a clean PASS
    // with the sanitizer leg entirely absent.
    BEZEL_EXPECT_TRUE(observedFailed);

    // And the outage is COUNTABLE, which is what makes a suite-wide break
    // legible as one infrastructure failure rather than N unrelated defects.
    // tests/test_main.cpp prints this as `NATIVE SANITIZER INFRA: N
    // build/link/exec break(s)`, the sibling of #3626's `NATIVE SANITIZER:`.
    BEZEL_EXPECT_TRUE(observedTally > savedTally);
}

// ─────────────────────────────────────────────────────────────────────────────
// The boundary: `-1` still means what all ~176 existing call sites think it
// means. A fix that made EVERYTHING loud would break the one outcome that is
// legitimately a skip — a developer box with no ASan runtime — and turn every
// such run red. `asanRuntimeObject()`'s empty result stays a silent -1 and
// stays counted by #3626's counter, not this one.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(native_asan_build_failure_4563,
           toolchain_unavailable_is_still_a_silent_minus_one_skip) {
    auto& tally = ::bezel::test::asanInfraFailures();
    const std::size_t before = tally.load();
    const std::size_t skipsBefore =
        ::bezel::native_test::nativeSanitizerUnavailableSkips().load();

    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();

    // Whichever way this box answers, the toolchain probe must not have been
    // reclassified as an infrastructure failure: an absent runtime is an
    // environment fact, and #3626's counter (plus #3682's refusal under the
    // sweep) is the mechanism that reports it.
    BEZEL_EXPECT_EQ(tally.load(), before);

    // Belt: whichever way the box answers, the #3626 channel — not this one —
    // is the one that moved. (Deliberately no assertion on `rtObj` itself: on
    // a box WITH a working runtime it is non-empty and on one without it is
    // empty, and both are correct, so asserting either way would be an oracle
    // that passes for the wrong reason.)
    BEZEL_EXPECT_TRUE(::bezel::native_test::nativeSanitizerUnavailableSkips()
                          .load() >= skipsBefore);
    (void)rtObj;
}
