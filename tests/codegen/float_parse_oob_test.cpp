// Regression tests — out-of-bounds reads in the binary-float Parse family.
//
// WHAT THE DOCUMENTS REQUIRE (stdlib §9.2, v0.314 — "Input grammar"):
//   `Parse` accepts an optional sign, then one or more digits, then optionally
//   a '.' with digits, then optionally an exponent, "and nothing else".
//   `".5"`, `"5."`, `"0x1p3"` and any other unadmitted input raise
//   `FormatException`.  `TryParse` returns `false` in every case where `Parse`
//   would raise; `Convert.ToDouble` / `Convert.TryToDouble` (stdlib §2.3)
//   carry the same contract.
//
//   The compiler must reach that FormatException WITHOUT reading outside the
//   string's buffer.  A fault is still a path: raising on invalid input is not
//   permission to inspect memory the input does not contain.
//
// MEASURED PRE-FIX (r23-c28 @ 3fa8d57c), ASan-instrumented generated program:
//   Two distinct out-of-bounds reads, both introduced by #5399 (ee560b0e) and
//   #5354 (cd8eba59), both on the reject path of an invalid input:
//
//   1. UNDER-READ, index -1.  The §9.2 trailing-token check loads
//      `data + (endptr - data) - 1`.  When strtod consumes nothing — which is
//      exactly what it does for "abc", "x", ".", "-", "+", "e5", " " and "" —
//      `endptr == data`, so the offset is `0 - 1` and the load reads one byte
//      BEFORE the string's first byte:
//        ERROR: AddressSanitizer: global-buffer-overflow
//        READ of size 1 at 0x00000055a03f
//        0x00000055a03f is located 1 bytes before global variable
//          '.bz.str.data.52' ... of size 4  '.bz.str.data.52' is ascii 'abc'
//
//   2. OVER-READ, index +1.  The hexadecimal-float check loads `data[1]`
//      unconditionally.  The empty string's buffer is exactly the 1-byte NUL
//      terminator, so that load reads one byte PAST the global:
//        0x...c1 is located 0 bytes after global variable '.bz.str.data.51'
//          defined in 'bezel' of size 1
//
//   Neither is observable without a sanitizer (the bad bytes are ignored), so
//   every row below runs under `runAotChecked` — the ASan/LSan oracle.  Each
//   asserts `r.ran` POSITIVELY: a toolchain skip makes the row FAIL rather
//   than silently pass (the #3733 absorbing-skip defect).
//
// THE FIX: both loads degrade their index instead of trusting it.  The
// trailing-token index is `select(advanced, consumed - 1, 0)` — when strtod
// consumed nothing there is no last consumed character, and `advanced` already
// forces the input invalid, so the byte read is immaterial and index 0 is in
// bounds.  The hex-indicator index is `select(isLeadingZero, 1, 0)` — a hex
// form requires a leading '0', so on any other first byte the value is ANDed
// away and index 0 is read instead.  No defined input changes value; the rows
// at the end pin that the admitted inputs still parse to their values.
//
// MEASURED POST-FIX (r23-c28 + this commit): a matrix of 10 entry points
// (Convert.ToDouble / Convert.TryToDouble / Convert.ToInt64 / double & float
// Parse / TryParse, string and span forms) x 16 inputs (the empty string,
// "abc", ".", "5.", "x", "e5", "-", "+", "0x1p3", "1e", " ", "nan", "inf",
// "0.5", "1e999", "  5") reports ZERO buffer overflows, down from 24 of 52 on
// the pre-fix compiler.  26 of the 160 still report a LeakSanitizer finding,
// all of them the unrelated span-Parse throw-path leak described below.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Wrap `body` in a Main that returns `body`'s value, and assert the generated
// program ran to exactly `want` under the ASan/LSan oracle.  `r.ran` is
// asserted positively: if the toolchain were unavailable the row fails rather
// than reporting a vacuous pass.
void expectExitChecked(const std::string& body, int want) {
    auto r = runAotChecked(std::string("module M; class C public static method Main(): int ") +
                           body + " end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, want);
}

// A `Parse` / `ToDouble` row: `stmt` must raise FormatException (-> 1) and
// must not trip the sanitizer.  Without the fix the generated program aborts
// inside ASan with a global-buffer-overflow report and never returns 1
// cleanly, so the oracle reports the row as a failure.
#define BEZEL_FLOAT_OOB_THROWS(suffix, stmt)                                 \
    BEZEL_TEST(float_parse_oob, suffix) {                                   \
        expectExitChecked("try " stmt " return 0; "                          \
                          "catch FormatException => return 1; end; ", 1);   \
    }

// A `TryParse` / `TryToDouble` row: must return false (-> 1).
#define BEZEL_FLOAT_OOB_TRY(suffix, expr)                                    \
    BEZEL_TEST(float_parse_oob, suffix) {                                   \
        expectExitChecked("var r: double := 0.0; var ok := " expr "; "       \
                          "return (if ok then 0 else 1); ", 1);             \
    }

}  // namespace

// ── The under-read: an input strtod consumes nothing of ─────────────────────
// §9.2 admits none of these; each must raise / return false, and none may read
// index -1 of the buffer.

BEZEL_FLOAT_OOB_THROWS(to_double_abc, "var x := Convert.ToDouble(\"abc\");")
BEZEL_FLOAT_OOB_THROWS(to_double_dot, "var x := Convert.ToDouble(\".\");")
BEZEL_FLOAT_OOB_THROWS(double_parse_abc, "var x := double.Parse(\"abc\");")
BEZEL_FLOAT_OOB_THROWS(double_parse_x, "var x := double.Parse(\"x\");")
BEZEL_FLOAT_OOB_THROWS(double_parse_e5, "var x := double.Parse(\"e5\");")
BEZEL_FLOAT_OOB_THROWS(double_parse_minus, "var x := double.Parse(\"-\");")
BEZEL_FLOAT_OOB_THROWS(double_parse_space, "var x := double.Parse(\" 5\");")
BEZEL_FLOAT_OOB_THROWS(float_parse_abc, "var x := float.Parse(\"abc\");")

// Empty string too — strtod consumes nothing, so it under-reads index -1 as
// well as over-reading index +1 below.
BEZEL_FLOAT_OOB_THROWS(to_double_empty, "var x := Convert.ToDouble(\"\");")
BEZEL_FLOAT_OOB_THROWS(double_parse_empty, "var x := double.Parse(\"\");")
BEZEL_FLOAT_OOB_THROWS(float_parse_empty, "var x := float.Parse(\"\");")

BEZEL_FLOAT_OOB_TRY(try_to_double_abc, "Convert.TryToDouble(\"abc\", out r)")
BEZEL_FLOAT_OOB_TRY(try_to_double_empty, "Convert.TryToDouble(\"\", out r)")
BEZEL_FLOAT_OOB_TRY(double_try_parse_abc, "double.TryParse(\"abc\", out r)")
BEZEL_FLOAT_OOB_TRY(double_try_parse_empty, "double.TryParse(\"\", out r)")
BEZEL_FLOAT_OOB_TRY(float_try_parse_abc, "float.TryParse(\"abc\", out r)")
BEZEL_FLOAT_OOB_TRY(float_try_parse_empty, "float.TryParse(\"\", out r)")

// ── The span overloads (stdlib §9.2 / #1810) — same contract, and the empty
// window's buffer is the 1-byte NUL terminator alone. ───────────────────────
//
// NOTE ON COVERAGE GAPS (deliberate, not an oversight):
//   The `double.Parse(span)` / `float.Parse(span)` arms are fixed here too —
//   the span window takes the same two clamped loads.  They are not pinned by
//   a runAotChecked row because a SEPARATE, PRE-EXISTING defect makes that
//   impossible today, and this commit does not fix it: on the throwing path
//   `emitGuardThrow` leaves the block before
//   `if (win.owned) emitStringRelease(win.owned);` runs, so the materialized
//   window leaks.  MEASURED (r23-c28 @ 3fa8d57c): valid span Parse and invalid
//   span TryParse do NOT leak; invalid span Parse leaks 33–36 bytes from
//   `bz_str_slice_cp`.  An ASan row for it therefore fails on the leak, not on
//   the out-of-bounds read, and would be a vacuous pin for this defect.  The
//   TryParse span arm below returns rather than throwing, so it does not touch
//   that path and does pin the empty-window over-read.  Reported separately.

BEZEL_TEST(float_parse_oob, double_try_parse_span_empty) {
    expectExitChecked("var sp := \"\".AsSpan(0, 0); var r: double := 0.0; "
                      "var ok := double.TryParse(sp, out r); "
                      "return (if ok then 0 else 1); ", 1);
}

BEZEL_TEST(float_parse_oob, double_try_parse_span_abc) {
    expectExitChecked("var sp := \"abc\".AsSpan(0, 3); var r: double := 0.0; "
                      "var ok := double.TryParse(sp, out r); "
                      "return (if ok then 0 else 1); ", 1);
}

BEZEL_TEST(float_parse_oob, float_try_parse_span_empty) {
    expectExitChecked("var sp := \"\".AsSpan(0, 0); var r: float := 0.0f; "
                      "var ok := float.TryParse(sp, out r); "
                      "return (if ok then 0 else 1); ", 1);
}

// ── The clamp must not disarm the checks it guards ──────────────────────────
// These are the inputs that make each clamped load take its ORIGINAL branch,
// so they fail if the select is wired backwards or the byte is read from the
// wrong index.

// "0x1p3": first byte '0' so the hex-indicator load MUST read index 1 and see
// 'x'.  §9.2 admits no hexadecimal form -> FormatException.
BEZEL_TEST(float_parse_oob, hex_float_still_rejected_leading_zero) {
    expectExitChecked("try var x := double.Parse(\"0x1p3\"); return 0; "
                      "catch FormatException => return 1; end; ", 1);
}

BEZEL_TEST(float_parse_oob, hex_float_still_rejected_to_double) {
    expectExitChecked("try var x := Convert.ToDouble(\"0X1p3\"); return 0; "
                      "catch FormatException => return 1; end; ", 1);
}

// "5." / "1e": strtod consumes one byte, so the trailing-token load MUST read
// index 0 and see the rejected '.' / 'e'.
BEZEL_TEST(float_parse_oob, trailing_dot_still_rejected) {
    expectExitChecked("try var x := double.Parse(\"5.\"); return 0; "
                      "catch FormatException => return 1; end; ", 1);
}

BEZEL_TEST(float_parse_oob, trailing_exponent_still_rejected) {
    expectExitChecked("try var x := double.Parse(\"1e\"); return 0; "
                      "catch FormatException => return 1; end; ", 1);
}

// ── And the admitted inputs still parse to their values ────────────────────
// A clamp that always degraded to index 0 would pass every rejection row above
// while breaking parsing; these pin the values.

BEZEL_TEST(float_parse_oob, valid_double_still_parses) {
    expectExitChecked("var x := double.Parse(\"3.25\"); "
                      "return (if x = 3.25 then 1 else 0); ", 1);
}

BEZEL_TEST(float_parse_oob, valid_float_still_parses) {
    expectExitChecked("var x := float.Parse(\"3.25\"); "
                      "return (if x = 3.25 then 1 else 0); ", 1);
}

BEZEL_TEST(float_parse_oob, valid_to_double_still_parses) {
    expectExitChecked("var x := Convert.ToDouble(\"42.5\"); "
                      "return (if x = 42.5 then 1 else 0); ", 1);
}

BEZEL_TEST(float_parse_oob, valid_hex_prefix_zero_still_parses) {
    // "0.5" — first byte '0', so the hex load reads index 1 ('.'), not 'x',
    // and the value must be admitted.
    expectExitChecked("var x := double.Parse(\"0.5\"); "
                      "return (if x = 0.5 then 1 else 0); ", 1);
}

BEZEL_TEST(float_parse_oob, valid_span_still_parses) {
    expectExitChecked("var sp := \"3.14\".AsSpan(0, 4); var x := double.Parse(sp); "
                      "return (if x = 3.14 then 1 else 0); ", 1);
}

BEZEL_TEST(float_parse_oob, valid_try_parse_returns_true) {
    expectExitChecked("var r: double := 0.0; "
                      "var ok := double.TryParse(\"7.5\", out r); "
                      "return (if ok and r = 7.5 then 1 else 0); ", 1);
}

BEZEL_TEST(float_parse_oob, valid_try_to_double_returns_true) {
    expectExitChecked("var r: double := 0.0; "
                      "var ok := Convert.TryToDouble(\"7.5\", out r); "
                      "return (if ok and r = 7.5 then 1 else 0); ", 1);
}

// §9.2: the ±Infinity / NaN spellings stand outside the grammar and are
// admitted; the clamp must not reject them.
BEZEL_TEST(float_parse_oob, infinity_spelling_still_accepted) {
    expectExitChecked("var x := double.Parse(\"Infinity\"); "
                      "return (if x > 1.0e308 then 1 else 0); ", 1);
}

// ── Regression: #5443 — span-Parse throw-path leak ──────────────────────────
//
// WHAT THE DEFECT IS:
//   double.Parse(span) / float.Parse(span) on invalid input (e.g. "abc") raises
//   FormatException but leaks the materialized window from bz_str_slice_cp.
//   The leak is 33-36 bytes per call — the owned window buffer.  §28.3 (v0.303)
//   requires every owned temporary to be discharged on every exit path.
//
// PRE-FIX MEASURED (r23-c28 @ 3fa8d57c, runAotChecked, 200 iterations):
//   LeakSanitizer: detected 6600 bytes leaked in 200 allocation(s)
//   Without the loop the one-shot repro would often pass (below LSan's threshold
//   for solo allocations), so the row loops 200x (50-500x as specified).
//
// THE FIX:
//   Release the window before emitGuardThrow rather than after it, so that both
//   the FormatException and OverflowException throwing paths release it before
//   branching to the error block.

BEZEL_TEST(float_parse_oob, double_parse_span_throw_path_no_leak) {
    // 200 iterations of double.Parse(span) with invalid input, each catching
    // FormatException.  On the pre-fix compiler LSan reports ~6600 bytes leaked.
    // On the fixed compiler: zero leaks, exit 0.
    expectExitChecked(
        "var sp := \"abc\".AsSpan(0, 3); "
        "var r := 0; "
        "for var i in 0..199 do "
        "    try var x := double.Parse(sp); r := 0; "
        "    catch FormatException => r := 1; "
        "    end try; "
        "end for; "
        "return r; ",
        1);
}

BEZEL_TEST(float_parse_oob, float_parse_span_throw_path_no_leak) {
    // Same for float.Parse(span).
    expectExitChecked(
        "var sp := \"abc\".AsSpan(0, 3); "
        "var r := 0; "
        "for var i in 0..199 do "
        "    try var x := float.Parse(sp); r := 0; "
        "    catch FormatException => r := 1; "
        "    end try; "
        "end for; "
        "return r; ",
        1);
}