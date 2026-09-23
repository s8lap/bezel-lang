#pragma once

// force_sanitize.hpp — THE definition of the whole-suite ASan sweep switch.
//
// `scripts/gate.sh --full` puts the whole suite under generated-code
// AddressSanitizer by exporting `BEZEL_AOT_FORCE_SANITIZE=address` (#1822).
// Three places need to know that: `runAot` (support/aot_support.hpp), the
// hand-rolled `Driver::compileFile` runners (support/driver_run_support.hpp),
// and the shard-summary writer that records which oracle a shard ran under
// (test_main.cpp). They had three independent copies of the parse, which is a
// drift channel with no compiler check on it — and the drift is not theoretical:
// the runners had no copy at all, which is precisely the hole
// driver_run_support.hpp exists to close.
//
// This header is deliberately DEPENDENCY-FREE (<cstdlib> + <string>) so
// test_main.cpp — which otherwise pulls in nothing from the compiler — can
// include it without dragging in Driver.hpp and LLVM.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace bezel::test {

// True when the whole-suite sweep is on: BEZEL_AOT_FORCE_SANITIZE set to
// "address" / "asan" / "1". Unset/empty (the default) leaves normal runs
// byte-for-byte unchanged. Memoized once per process — the environment does not
// change under a running test binary, and the memo keeps this callable from a
// hot path.
inline bool forceSanitizeAddress() {
    static const bool v = [] {
        const char* e = std::getenv("BEZEL_AOT_FORCE_SANITIZE");
        if (!e || *e == '\0') return false;
        std::string s(e);
        return s == "address" || s == "asan" || s == "1";
    }();
    return v;
}

// ---------------------------------------------------------------------------
// BEZEL_AOT_DETECT_LEAKS — the OPT-IN leak oracle for `runAot` (issue #4638).
//
// ── WHAT WAS MEASURED ──────────────────────────────────────────────────────
//
// `runAot` runs every sanitized program with
//     ASAN_OPTIONS=detect_leaks=0:abort_on_error=0      (aot_support.hpp)
// so a generated program can leak and the row stays green EVEN UNDER
// `gate.sh --full`. Measured 2026-09-23 on the #4619 membership-boxing
// program (500 boxed queries, 24 B apiece):
//
//   runAotChecked(leak500)                      -> LSan FIRES, 12000 B / 500 obj
//   runAot(leak500, sanitizeAddress = true)     -> clean, exit 2
//   runAot(leak500)                             -> clean, exit 2
//   bezelc --sanitize=address + bare run        -> LSan FIRES, 12000 B / 500 obj
//
// So the leak-checking MECHANISM works — `runAotChecked` (the harness whose
// documented contract is dispose-exactly-once) does check, and can fail. What
// does not check is `runAot`, whose contract never claimed to.
//
// ── WHY `runAot` IS NOT SIMPLY FLIPPED TO detect_leaks=1 ───────────────────
//
// Three reasons, all still valid, none of them an oversight (#1822, #4532):
//
//   1. §28.2 checked-disposal QUARANTINES disposed objects instead of freeing
//      them, on purpose (`bz_array_mark_disposed` &c., issues #621/#627) — "a
//      deliberate memory leak in checked builds". Every checked-disposal
//      program leaks under LSan, correctly.
//   2. A negative test that exits via an uncaught exception legitimately
//      leaves heap live at exit.
//   3. LSan's `exitcode` SUPERSEDES the program's own exit status. #4532
//      burned a gate cycle on exactly this: 7 rows asserting a value saw 1
//      instead, and one row asserting 1 passed vacuously.
//
// ── WHAT THIS KNOB DOES INSTEAD ────────────────────────────────────────────
//
//   unset / "0" / "off"   Off     — byte-for-byte today's behaviour.
//   "report" / "census"   Report  — LSan ON; a leak is COUNTED and printed as
//                                   a `[ LEAK ]` marker on stdout, between the
//                                   owning row's `[ RUN  ]` and `[  OK  ]`
//                                   lines, and the row's verdict is UNCHANGED.
//                                   This is the census instrument
//                                   (scripts/check_leak_census.py).
//   "1" / "strict" / "on" Strict  — LSan ON; a leak FAILS the row with a
//                                   distinguishable diagnostic.
//
// Report mode is verdict-preserving by construction, which is what makes it
// safe to switch on across the whole suite: `exitcode=0` stops LeakSanitizer
// replacing the program's exit status (reason 3 above), and the harness
// consumes the leak report rather than letting the #1822 ASan-report check
// see it. VERIFIED 2026-09-23 on the leak500 program:
//     detect_leaks=1:abort_on_error=0            -> exit 1  (program returns 2)
//     detect_leaks=1:exitcode=0:abort_on_error=0 -> exit 2, report still printed
//
// Either non-Off mode IMPLIES `opts.sanitizeAddress`: LeakSanitizer here is
// the ASan runtime's exit-time check, so without the ASan link there is
// nothing to turn on.
//
// DELIBERATELY NOT MEMOIZED, unlike `forceSanitizeAddress` above. The
// regression test (tests/support/aot_leak_detection_4638_test.cpp) toggles the
// variable at runtime with setenv() to exercise all three modes inside one
// process, exactly as `forceToolchainMissing()` is left unmemoized for
// tests/support/native_toolchain_unavailable_loud_test.cpp. One getenv is
// irrelevant beside a fork + compile + link + exec.
// ---------------------------------------------------------------------------
enum class LeakDetectMode { Off, Report, Strict };

inline LeakDetectMode leakDetectMode() {
    const char* e = std::getenv("BEZEL_AOT_DETECT_LEAKS");
    if (!e || *e == '\0') return LeakDetectMode::Off;
    const std::string s(e);
    if (s == "0" || s == "off" || s == "false") return LeakDetectMode::Off;
    if (s == "report" || s == "census") return LeakDetectMode::Report;
    if (s == "1" || s == "strict" || s == "on" || s == "true")
        return LeakDetectMode::Strict;
    // FAIL CLOSED on a typo. An unrecognised value silently meaning Off is the
    // vacuous-oracle shape this whole issue is about: the operator believes the
    // leak oracle is armed and it is not. Escalate to the STRICTEST reading and
    // say so once, so a misspelling is loud rather than invisible.
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
        std::fprintf(stderr,
                     "[bezel-tests] BEZEL_AOT_DETECT_LEAKS=\"%s\" is not one of "
                     "off|report|strict — reading it as `strict` (fail-closed, "
                     "#4638).\n",
                     s.c_str());
    return LeakDetectMode::Strict;
}

// Process-wide tally of leak reports observed by the AOT harness under either
// non-Off mode. Modelled on `nativeSanitizerUnavailableSkips()` /
// `checkedToolchainSkips()`: one counting chokepoint, so "how many rows leaked"
// is a number rather than a log-grep. Read by the census script through the
// `[ LEAK ]` markers; exposed here so a later change can also publish it in the
// shard summary without moving the definition.
inline std::atomic<std::size_t>& aotLeakDetections() {
    static std::atomic<std::size_t> n{0};
    return n;
}

// §22.16 / issue #4386: the suite-wide checked-contracts override, parallel
// to `forceSanitizeAddress` above. When `BEZEL_AOT_FORCE_CHECKED_CONTRACTS`
// is set to `1` / `on` / `true`, every `runAot` call sets
// `opts.checkedContracts = true` so the suite runs under checked-contracts
// mode uniformly — used by the gate's "force the whole suite under
// checked-contracts" sweep, mirroring the ASan sweep above. Unset/empty (the
// default) leaves `runAot`'s own test default in effect.
inline bool forceCheckedContracts() {
    static const bool v = [] {
        const char* e = std::getenv("BEZEL_AOT_FORCE_CHECKED_CONTRACTS");
        if (!e || *e == '\0') return false;
        std::string s(e);
        return s == "1" || s == "on" || s == "true";
    }();
    return v;
}

}  // namespace bezel::test
