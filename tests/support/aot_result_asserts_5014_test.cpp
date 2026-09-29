// issue #5014 — `failedAtCompileTimeWithRealLocation`'s sanitizer-fired
// exclusion list is NOT a vacuous-pass oracle hole.
//
// ── WHAT WAS CLAIMED ───────────────────────────────────────────────────────
//
// Issue body: under `BEZEL_AOT_FORCE_SANITIZE=address` (the gate's `--full`
// leg), a program that compiles and trips ASan at runtime reports
// `ran == false`. If the captured sanitizer stderr contains a
// `file:line:col: ` frame matching the helper's location regex, the helper
// returns `true` — certifying a runtime fault as a compile-time rejection.
// The exclusion list checked only `"AddressSanitizer fired"`, which the
// body claimed was never produced.
//
// ── WHAT WAS MEASURED (issue #5014 lane, 2026-09-26) ──────────────────────
//
// 1. The exclusion is live for the runAot path's 3372 emission
//    (`runAot: AddressSanitizer fired in the test program`) — the
//    correction comment on the issue established this.
//
// 2. The exclusion is DEAD for the runAotChecked path's 3700 emission
//    (`runAotChecked: sanitizer fired (…)`) — needle `"AddressSanitizer
//    fired"` does NOT match `"sanitizer fired"` (capital vs. lowercase S).
//
// 3. THE HELPER DOES NOT REACH THAT BRANCH. The location regex
//    `R"(:(\d+):(\d+): )"` requires THREE colons (a driver-style
//    `path:line:col: error` frame). ASan reports use the 2-colon
//    `file:line:col` format (`#0 0x… in main prog.bzl:5:11`,
//    `SUMMARY: AddressSanitizer: heap-buffer-overflow prog.bzl:5:11`),
//    which never matches the regex — measured 2026-09-26 on a real ASan
//    report produced by `clang++ -fsanitize=address` (see commit body for
//    the exact sample): the helper returns `false` for every reachable
//    sanitizer-fired emission (3372, 3700, 3357) regardless of the
//    exclusion list, because the regex itself never matches.
//
// 4. The two banner emission sites (aot_support.hpp:711 and
//    driver_run_support.hpp:206) print to fd 2; the helper only scans
//    `r.diagnostics.diagnostics()`, so they are outside the helper's view.
//
// ── WHY THIS IS THE RIGHT CLOSE ─────────────────────────────────────────────
//
// The exclusion list is belt-and-braces for an outcome that the location
// regex already excludes structurally: even if the needle never matches
// the emission prefix, the regex's 3-colon requirement means a sanitizer
// frame alone cannot satisfy the helper's positive-side check. The fix
// proposed by the issue body (a single needle `"sanitizer fired"`) does
// not even cover the runAot 3372 path — `"sanitizer fired"` (lowercase)
// is NOT a substring of `"AddressSanitizer fired"` (capital `S`), verified
// by direct substring search. Applying that fix would introduce a
// regression on the one path the existing needle does cover.
//
// ── WHAT THIS FILE PINS ─────────────────────────────────────────────────────
//
// Four unit tests over hand-built `JitResult`s, each exercising the helper
// on the EXACT diagnostic text the corresponding emission site produces
// (no harness fork, no clang link, no env var required):
//
//   * the runAot 3372 ASan emission with a realistic stack frame
//     (`prog.bzl:5:11`, 2-colon ASan format)
//   * the runAotChecked 3700 ASan emission with the same
//   * the runAot strict-leak 3357 emission (audit-found, gate-orthogonal)
//   * a genuine driver-style `CompileOutcome::Rejected` (`prog.bzl:5:9:
//     cannot apply `+` to …`, 3-colon driver format)
//
// The first three must return `false` (the regex never matches ASan
// frames); the fourth must return `true` (the regex matches driver
// rejections). If a future change loosens the regex to match ASan frames
// (e.g. `:(\d+):(\d+)` instead of `:(\d+):(\d+): `), the first three tests
// fail and the regression is caught — that is the file's purpose.
//
// All tests are independent of `BEZEL_AOT_FORCE_SANITIZE` and of clang
// availability — they exercise the helper as a pure function on
// `JitResult`, the only signature this file needs.

#include "test_harness.hpp"
#include "support/aot_result_asserts.hpp"

#include "support/Diagnostic.hpp"

namespace {

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;

// Build a `JitResult` with `ran=false` and ONE diagnostic carrying `msg`.
JitResult jitResultWithOneDiag(const std::string& msg) {
    JitResult r;
    r.diagnostics.report(bezel::Severity::Error, bezel::SourceLocation{}, msg);
    return r;
}

// ── 1. runAot 3372 ASan emission — regex does not match ────────────────────
//
// Real ASan frame format: `… in main prog.bzl:5:11\n` (TWO colons, then a
// newline — NOT a space). The helper's `R"(:(\d+):(\d+): )"` requires
// THREE colons followed by a space, which a driver-style rejection has and
// ASan does not.
BEZEL_TEST(aot_result_asserts_5014,
           run_aot_3372_asan_frame_is_not_a_compile_rejection) {
    const std::string msg =
        "runAot: AddressSanitizer fired in the test program\n"
        "=================================================================\n"
        "==1720339==ERROR: AddressSanitizer: heap-buffer-overflow on address "
        "0x7c19b3be001c at pc 0x0000004eb392 bp 0x7ffd32604350 sp "
        "0x7ffd32604348\n"
        "WRITE of size 1 at 0x7c19b3be001c thread T0\n"
        "    #0 0x0000004eb391 in main prog.bzl:5:11\n"
        "    #1 0x7ff9b4b1c5b4 in __libc_start_call_main (/lib64/libc.so.6"
        "+0x35b4)\n"
        "0x7c19b3be001c is located 0 bytes after 12-byte region "
        "[0x7c19b3be0010,0x7c19b3be001c)\n"
        "allocated by thread T0 here:\n"
        "    #0 0x0000004a5bd8 in malloc (/tmp/asan_repro+0x4a5bd8)\n"
        "    #1 0x0000004eb358 in main prog.bzl:4:22\n"
        "SUMMARY: AddressSanitizer: heap-buffer-overflow prog.bzl:5:11 in main\n";
    auto r = jitResultWithOneDiag(msg);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
}

// ── 2. runAotChecked 3700 ASan emission — regex does not match ──────────────
BEZEL_TEST(aot_result_asserts_5014,
           run_aot_checked_3700_asan_frame_is_not_a_compile_rejection) {
    const std::string msg =
        "runAotChecked: sanitizer fired (ASan at exit)\n"
        "--- sanitizer report ---\n"
        "ERROR: AddressSanitizer: heap-buffer-overflow on address "
        "0x7c19b3be001c at pc 0x0000004eb392 bp 0x7ffd32604350 sp "
        "0x7ffd32604348\n"
        "WRITE of size 1 at 0x7c19b3be001c thread T0\n"
        "    #0 0x0000004eb391 in main prog.bzl:5:11\n"
        "    #1 0x7ff9b4b1c5b4 in __libc_start_call_main (/lib64/libc.so.6"
        "+0x35b4)\n"
        "SUMMARY: AddressSanitizer: heap-buffer-overflow prog.bzl:5:11 in main\n";
    auto r = jitResultWithOneDiag(msg);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
}

// ── 3. runAot 3357 strict-leak emission — regex does not match ─────────────
//
// `BEZEL_AOT_DETECT_LEAKS=strict` is gate-orthogonal (the gate's `--full`
// leg does NOT set it), but the same regex argument applies: an LSan frame
// uses the same 2-colon `file:line:col` format.
BEZEL_TEST(aot_result_asserts_5014,
           run_aot_strict_leak_3357_lsan_frame_is_not_a_compile_rejection) {
    const std::string msg =
        "runAot: LeakSanitizer fired in the test program "
        "(BEZEL_AOT_DETECT_LEAKS=strict, #4638)\n"
        "ERROR: LeakSanitizer: detected memory leaks\n"
        "Direct leak of 24 byte(s) in 1 object(s) allocated from:\n"
        "    #0 0x0000004eb391 in main prog.bzl:5:11\n"
        "    #1 0x0000004a5bd8 in malloc (/tmp/asan_repro+0x4a5bd8)\n"
        "SUMMARY: LeakSanitizer: 24 byte(s) leaked in 1 allocation(s).\n";
    auto r = jitResultWithOneDiag(msg);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
}

// ── 4. Positive case — a genuine rejection IS recognised ───────────────────
//
// Regression guard against widening the regex (e.g. dropping the trailing
// space requirement). The driver's captured stderr is a single Diagnostic
// carrying the WHOLE stderr blob, with ONE `<path>:<line>:<col>: <message>`
// line per real diagnostic — the canonical input shape this helper exists
// for. If a future change makes the regex too loose, tests 1/2/3 above
// catch it; if the change makes the regex too strict, this row catches it.
BEZEL_TEST(aot_result_asserts_5014,
           genuine_compile_rejection_is_still_recognised) {
    const std::string msg =
        "prog.bzl:5:9: cannot apply `+` to operands of type "
        "`decimal` and `string` (#4807)\n";
    auto r = jitResultWithOneDiag(msg);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

}  // namespace
