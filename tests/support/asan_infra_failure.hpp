#pragma once

// asan_infra_failure.hpp — the ONE place the native-sanitizer leg says
// "my own setup broke", as distinct from "the toolchain isn't here" and from
// "the sanitizer found something".
//
// ── THE HOLE THIS CLOSES (#4563) ───────────────────────────────────────────
//
// Every hand-rolled `runUnderLeakSanitizer` / `runUnderAsan*` copy in this
// tree (~77 files, the #3917 family) opened with the same shape:
//
//     const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
//     if (rtObj.empty()) return -1;                       // (1) no toolchain
//     ...
//     if (!unit) return -1;                               // (2) parse failed
//     if (diags.hasErrors()) return -1;                   // (2) sema failed
//     if (!module) return -1;                             // (2) emitModule
//     if (!emitObjectFileForTest(...)) return -1;         // (2) emitObject
//     ...
//     if (std::system(linkCmd) != 0) { cleanup(); return -3; }   // (3) link
//
// and every caller then wrote
//
//     int lsan = runUnderLeakSanitizer(src);
//     if (lsan == -1) return;        // toolchain unavailable: skip the leg
//     BEZEL_EXPECT_EQ(lsan, 0);
//
// so THREE structurally different outcomes shared one sentinel. Only (1) is a
// legitimate skip. (2) is a failure of the test's OWN setup, and it reported a
// silent PASS: the row ran, printed `[ OK ]`, and asserted nothing about
// memory safety. `nativeSanitizerUnavailableSkips()` (#3626) — the counter
// that exists precisely so an unavailable oracle is "reported, not silent" —
// is incremented ONLY on (1), so a suite-wide outage of (2) was invisible to
// the acceptance gate.
//
// That was not a hypothetical. #3917's burn-down repointed these runners onto
// `bezel::test::emitObjectFileForTest`, which under `gate.sh --full` emits an
// ASan-INSTRUMENTED object. An instrumented object references `__asan_report_*`
// and registers an `asan.module_ctor`; had any runner's link line lacked
// `-fsanitize=address`, the link would have failed and the ENTIRE file's
// sanitizer leg would have vanished while every row stayed green. The batch had
// to be verified with a throwaway `nm` + link-status probe precisely because
// the helper itself could not tell the two apart.
//
// ── THE CONTRACT ───────────────────────────────────────────────────────────
//
// The canonical per-cause codes are the ones `tests/support/run_under_asan.hpp`
// has documented since #3633:
//
//     -1  toolchain unavailable                      -> legitimate SKIP
//     -2  the test PROGRAM failed to build           -> INFRA failure
//     -3  link failure under -fsanitize=address      -> INFRA failure
//     -4  system() failure (fork/exec-level)         -> INFRA failure
//      0  clean;  1 / raw exit code: family-dependent
//
// `-1` KEEPS its meaning, which is what lets the ~176 existing
// `if (rc == -1) return;` call sites stay correct and untouched. The point of
// this header is that -2/-3/-4 are (a) not equal to -1, so a skip guard no
// longer swallows them, and (b) LOUD — each prints an `[ INFRA ]` marker and
// marks the current test failed DIRECTLY.
//
// (b) is the load-bearing half, not (a). A distinct code alone is not enough:
// ~49 sites in this family guard with `if (rc < 0) return;` or
// `if (rc >= 0) BEZEL_EXPECT_EQ(rc, 0);` (the `swallowing_guards` count
// scripts/check_native_asan_leg_skips.py ratchets), and those swallow -2/-3/-4
// exactly as they swallowed -1. Marking the test failed at the CHOKEPOINT means
// a caller that ignores the new codes entirely still fails, so the fix does not
// depend on 176 call sites being individually re-audited.
//
// ── ONE SUPPRESSION MECHANISM, NOT TWO ─────────────────────────────────────
//
// `noteAsanInfraFailure` and `ExpectAsanInfraFailure` MOVED here verbatim from
// run_under_asan.hpp (#4504 / narrow #4504) — same namespace
// (`bezel::test::asan_detail`), same behaviour — so that header, its 112 call
// sites, and the #3676 ratchet that uses the guard to induce a link failure are
// all untouched. A test that must INDUCE an infra failure uses that one guard;
// there is deliberately no second way to suppress the marking.
//
// Kept dependency-light on purpose (`<atomic>`, `<iostream>`, `<string>` plus
// test_harness.hpp for `currentTestFailed()`), because native_test_harness.hpp
// includes it and therefore every file in the family does.

#include <atomic>
#include <cstddef>
#include <iostream>
#include <string>

#include "test_harness.hpp"  // bezel::test::currentTestFailed()

namespace bezel::test {

// ---------------------------------------------------------------------------
// asanInfraFailures — the process-wide count of native-sanitizer-leg
// INFRASTRUCTURE failures (build / link / exec), the (2)+(3) sibling of
// `native_test::nativeSanitizerUnavailableSkips()`'s (1).
//
// #4563: an infra failure already fails its own test, so the gate cannot miss
// ONE of them. This counter is about the SHAPE of the outage: a whole suite
// whose sanitizer leg stopped building looks, test by test, like N unrelated
// failures. tests/test_main.cpp prints this unconditionally in the per-binary
// summary (so "0" is a positive confirmation, not the absence of a signal) and
// emits it as an additive JSON field, exactly as #3626 and #3733 did for their
// counters.
//
// NOT incremented inside an `ExpectAsanInfraFailure` scope: the #3676 ratchet
// induces a link failure on purpose, and a deliberately induced failure is not
// an outage.
// ---------------------------------------------------------------------------
inline std::atomic<std::size_t>& asanInfraFailures() {
    static std::atomic<std::size_t> n{0};
    return n;
}

namespace asan_detail {

// #4504: print an `[ INFRA ]` marker naming the failure mode and mark the
// CURRENT test failed directly, so a build/link/system() failure cannot be
// misread as a sanitizer finding — or as a clean run — regardless of which
// assertion macro the caller uses on the returned code. Mirrors
// `noteInfraFailure` (tests/support/aot_support.hpp) / `sanitizerRuntimeMaster`'s
// `[ INFRA ]` marker (tests/native_test_harness.hpp) — same "print inline with
// BEZEL_EXPECT_* output, fail unconditionally" shape, extended here to the
// native-ASan-leg build/link/exec path those two don't cover.
//
// #4504's marking is unconditional for every REAL caller. The one test that
// must opt out is the #3676 ratchet, which INDUCES a link failure on purpose
// to pin the -3 return contract — for it, the marking is the mechanism under
// test, not a verdict on the test. `ExpectAsanInfraFailure` suppresses the
// marking for its scope and COUNTS the markers instead, so the ratchet asserts
// both halves (the code is -3 AND the marker fired) rather than losing one.
// Deliberately not a bare bool: a suppression that cannot be asserted against
// is a suppression that can hide a missing marker.
inline int& asanInfraFailureCount() {
    static thread_local int n = 0;
    return n;
}

inline bool& asanInfraFailureExpected() {
    static thread_local bool v = false;
    return v;
}

struct ExpectAsanInfraFailure {
    int before;
    ExpectAsanInfraFailure() : before(asanInfraFailureCount()) {
        asanInfraFailureExpected() = true;
    }
    ~ExpectAsanInfraFailure() { asanInfraFailureExpected() = false; }
    ExpectAsanInfraFailure(const ExpectAsanInfraFailure&) = delete;
    ExpectAsanInfraFailure& operator=(const ExpectAsanInfraFailure&) = delete;
    // How many `[ INFRA ]` markers fired inside this scope.
    int count() const { return asanInfraFailureCount() - before; }
};

// The general form. `what` names the failure class for the second banner line
// ("link/exec" for #4504's original two causes, "build" for #4563's).
inline void noteAsanInfraFailureOf(const char* what, const char* tag,
                                   const std::string& reason,
                                   const char* issue) {
    std::cout << "    [ INFRA ] runUnderAsan(tag=\"" << (tag ? tag : "?")
              << "\"): " << reason << "\n"
              << "    [ INFRA ] this is an INFRASTRUCTURE failure ("
              << what << "), NOT a sanitizer finding -- see " << issue << ".\n";
    std::cout.flush();
    ++asanInfraFailureCount();
    // Suppressed ONLY inside an ExpectAsanInfraFailure scope (the #3676
    // ratchet). Every other caller still fails unconditionally.
    if (!asanInfraFailureExpected()) {
        ::bezel::test::currentTestFailed() = true;
        ::bezel::test::asanInfraFailures().fetch_add(1,
                                                     std::memory_order_relaxed);
    }
}

// The #4504 spelling, unchanged in both wording and behaviour: run_under_asan.hpp
// and the #3676 ratchet call this.
inline void noteAsanInfraFailure(const char* tag, const std::string& reason) {
    noteAsanInfraFailureOf("link/exec", tag, reason, "#4504");
}

}  // namespace asan_detail

// ---------------------------------------------------------------------------
// The three call-site spellings a hand-rolled runner uses. Each ANNOUNCES the
// failure and RETURNS the canonical code, so a converted site is a one-token
// edit that cannot forget the loud half:
//
//     if (!unit)   return -1;                    // silent skip (the #4563 bug)
//  -> if (!unit)   return bezel::test::asanBuildFailure("parse");
//
// `stage` is a short literal naming which step failed ("parse", "sema",
// "emitModule", "emitObjectFile", "link", "system"), so a report identifies the
// break without a rebuild. It is diagnostic only; the returned code is the
// contract.
// ---------------------------------------------------------------------------

// -2: the test PROGRAM failed to build (parse / sema / emitModule /
// emitObjectFile). This is the outcome #4563 is named for — the one that used
// to share `-1` with "no toolchain" and so reported a silent PASS.
inline int asanBuildFailure(const char* stage, const char* tag = nullptr) {
    asan_detail::noteAsanInfraFailureOf(
        "build", tag,
        std::string("the test PROGRAM failed to build at stage \"") +
            (stage ? stage : "?") +
            "\"; the sanitizer leg did NOT run, so this row asserts nothing "
            "about memory safety",
        "#4563");
    return -2;
}

// -3: the link under `-fsanitize=address` failed. `captured` is the link
// command's stderr when the caller kept it (run_under_asan.hpp does; the
// per-file copies redirect to /dev/null and pass nothing).
inline int asanLinkFailure(const char* stage, const std::string& captured = {},
                           const char* tag = nullptr) {
    asan_detail::noteAsanInfraFailureOf(
        "link", tag,
        std::string("`clang++ -fsanitize=address` failed at stage \"") +
            (stage ? stage : "?") + "\"" +
            (captured.empty() ? std::string("; stderr was not captured")
                              : "; captured stderr:\n" + captured),
        "#4563");
    return -3;
}

// -4: `system()` could not run the linked program at all (fork/exec-level:
// OOM, disk, no fork slot, ...).
inline int asanExecFailure(const char* stage, const char* tag = nullptr) {
    asan_detail::noteAsanInfraFailureOf(
        "exec", tag,
        std::string("system() failed to invoke the test program at stage \"") +
            (stage ? stage : "?") +
            "\" (fork/exec-level failure: OOM, disk, no fork slot, ...)",
        "#4563");
    return -4;
}

}  // namespace bezel::test
