// Codegen — `DateOnly.Parse` and `TimeOnly.Parse` on a MALFORMED string must
// raise a CATCHABLE FormatException, not an uncatchable trap. Issue #2015 (HIGH
// — uncatchable crash where a catchable exception is specified). Exact mirror of
// #2009 (DateTime.Parse) and #2004 (Guid.Parse), same file, same mechanism; the
// sibling DateTime.Parse fix's own commit flagged these two as follow-up.
//
// Spec: stdlib §2.5 (v0.197) —
//   `public static method Parse(s: string): DateOnly;   // FormatException on invalid`
//   `public static method Parse(s: string): TimeOnly;`
//   Parse and range contracts (v0.195): "`TimeOnly.Parse` accepts `HH:mm:ss`
//   with the same optional `.fff` tail" and DateTime.Parse "any other input
//   raises `FormatException`, the `DateOnly.Parse` stance." DateOnly's own
//   rendered form is `yyyy-MM-dd` (its ToString). The strict-fault model
//   (error-model catchable-exceptions) makes any other input a *catchable*
//   FormatException, like every other builtin guard. Spec revision v0.197.
//
// THE DEFECT (base 19b6566). LlvmBackend_Calls.cpp lowered BOTH the
// DateOnly.Parse and TimeOnly.Parse failure edges to `CreateUnreachable()` — an
// uncatchable trap where the spec promises a catchable exception.
// `bz_datetime_try_parse` / `bz_timeonly_try_parse` return 0 on a malformed
// string, so `DateOnly.Parse("garbage")` / `TimeOnly.Parse("garbage")` branched
// to the `unreachable` block: a `catch FormatException` NEVER fired and
// execution fell through into undefined code → wild-pointer read (SIGSEGV under
// plain codegen; ASan "unknown-crash"). Every malformed shape hit it: garbage,
// empty, an out-of-range field, or a well-shaped prefix with trailing junk.
//
// THE FIX. Route each failure edge through the standard builtin-throw path —
// `emitGuardThrow(ok == 0, "FormatException")` — exactly the mechanism the
// sibling #2009 DateTime.Parse, #2004 Guid.Parse, and the Decimal.Parse /
// convert-from-string guards already use. The malformed fault is now a
// first-class, catchable throw. No runtime change: both try_parse functions
// already return 0 on failure, 1 on success.
//
// SCOPE. `DateOnly.TryParse` / `TimeOnly.TryParse` were NEVER affected — they
// lower via distinct paths that return the bool and never trap; they must STILL
// return false (never throw) on malformed input. Valid renderings must still
// parse successfully after the fix.
//
// OBSERVABILITY. On base 19b6566 each malformed-Parse case SIGSEGVs / ASan-
// crashes (ran=false); with the fix the FormatException is caught and the
// program exits cleanly. Verified fail-without/pass-with by building both ways.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// (A) THE HEADLINE — a malformed literal fed to DateOnly.Parse raises a CATCHABLE
// FormatException, so the program reaches `return 0`. On base 19b6566 the
// `unreachable` failure edge crashes (SIGSEGV) → runAot reports ran=false; the
// literal argument is immortal, so nothing leaks on either path.
const char* kDateOnlyMalformedIsCaught =
    "module M; class C "
    "  public static method parseRaises(s: string): bool "
    "    var raised := false; "
    "    try "
    "      _ := DateOnly.Parse(s); "
    "    catch FormatException => raised := true; "
    "    end; "
    "    return raised; "
    "  end parseRaises; "
    "  public static method Main(): int "
    "    if not parseRaises(\"garbage\") then return 1; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// (A2) THE HEADLINE for TimeOnly — same structure.
const char* kTimeOnlyMalformedIsCaught =
    "module M; class C "
    "  public static method parseRaises(s: string): bool "
    "    var raised := false; "
    "    try "
    "      _ := TimeOnly.Parse(s); "
    "    catch FormatException => raised := true; "
    "    end; "
    "    return raised; "
    "  end parseRaises; "
    "  public static method Main(): int "
    "    if not parseRaises(\"garbage\") then return 1; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// (B) Every malformed DateOnly shape raises a catchable FormatException, and the
// valid `yyyy-MM-dd` rendering does NOT. Leak-free (immortal literals; a caught
// FormatException is freed by the catch machinery). On base the FIRST case
// crashes; with the fix returns 88.
const char* kDateOnlyAllMalformedShapes =
    "module M; class C "
    "  public static method parseRaises(s: string): bool "
    "    var raised := false; "
    "    try "
    "      _ := DateOnly.Parse(s); "
    "    catch FormatException => raised := true; "
    "    end; "
    "    return raised; "
    "  end parseRaises; "
    "  public static method Main(): int "
    "    if not parseRaises(\"garbage\") then return 1; end; "                       // non-numeric junk
    "    if not parseRaises(\"\") then return 2; end; "                              // empty
    "    if not parseRaises(\"2024-13-01\") then return 3; end; "                    // month 13 out of range
    "    if not parseRaises(\"2024-01-00\") then return 4; end; "                    // day 0 out of range
    "    if not parseRaises(\"2024-03-15xtrailing\") then return 5; end; "           // valid prefix + junk
    "    if not parseRaises(\"2024/03/15\") then return 6; end; "                    // wrong separators
    "    if not parseRaises(\"not-a-date\") then return 7; end; "                    // alpha junk
    "    if parseRaises(\"2024-03-15\") then return 20; end; "                       // valid yyyy-MM-dd
    "    if parseRaises(\"2025-06-15\") then return 21; end; "                       // valid yyyy-MM-dd
    "    return 88; "
    "  end Main; "
    "end C;";

// (B2) Every malformed TimeOnly shape raises; the valid `HH:mm:ss` and `.fff`
// renderings do not. Returns 77.
const char* kTimeOnlyAllMalformedShapes =
    "module M; class C "
    "  public static method parseRaises(s: string): bool "
    "    var raised := false; "
    "    try "
    "      _ := TimeOnly.Parse(s); "
    "    catch FormatException => raised := true; "
    "    end; "
    "    return raised; "
    "  end parseRaises; "
    "  public static method Main(): int "
    "    if not parseRaises(\"garbage\") then return 1; end; "                       // non-numeric junk
    "    if not parseRaises(\"\") then return 2; end; "                              // empty
    "    if not parseRaises(\"25:00:00\") then return 3; end; "                      // hour 25 out of range
    "    if not parseRaises(\"13:60:00\") then return 4; end; "                      // minute 60 out of range
    "    if not parseRaises(\"13:45:60\") then return 5; end; "                      // second 60 out of range
    "    if not parseRaises(\"13:45\") then return 6; end; "                         // too short (needs HH:mm:ss)
    "    if not parseRaises(\"13:45:30.50\") then return 7; end; "                   // 2-digit .ff (needs 3)
    "    if not parseRaises(\"13:45:30xy\") then return 8; end; "                    // valid prefix + junk
    "    if not parseRaises(\"13/45/30\") then return 9; end; "                      // wrong separators
    "    if parseRaises(\"13:45:30\") then return 20; end; "                         // valid HH:mm:ss
    "    if parseRaises(\"13:45:30.500\") then return 21; end; "                     // valid .fff
    "    return 77; "
    "  end Main; "
    "end C;";

// (C) The fix did not break VALID parsing — the DateOnly `yyyy-MM-dd` and
// TimeOnly `HH:mm:ss` / `.fff` renderings still parse to the right value (fields
// read back exact, per §2.5). Leak-free literals. Returns 55.
const char* kValidStillParses =
    "module M; class C "
    "  public static method Main(): int "
    "    var d := DateOnly.Parse(\"2024-03-15\"); "
    "    if d.Year <> 2024 then return 1; end; "
    "    if d.Month <> 3 then return 2; end; "
    "    if d.Day <> 15 then return 3; end; "
    "    var t := TimeOnly.Parse(\"13:45:30\"); "
    "    if t.Hour <> 13 then return 4; end; "
    "    if t.Minute <> 45 then return 5; end; "
    "    if t.Second <> 30 then return 6; end; "
    "    if t.Millisecond <> 0 then return 7; end; "
    "    var f := TimeOnly.Parse(\"13:45:30.500\"); "
    "    if f.Hour <> 13 then return 8; end; "
    "    if f.Millisecond <> 500 then return 9; end; "
    "    return 55; "
    "  end Main; "
    "end C;";

// (D) DateOnly.TryParse / TimeOnly.TryParse are UNAFFECTED — malformed input
// returns false (never throws, never crashes) and a valid string returns true
// with the parsed value. These paths never trapped; the assertion pins that the
// #2015 fix left them alone. Returns 33.
const char* kTryParseUnaffected =
    "module M; class C "
    "  public static method Main(): int "
    "    var d1: DateOnly; "
    "    var ok1 := DateOnly.TryParse(\"garbage\", out d1); "
    "    if ok1 then return 1; end; "
    "    var d2: DateOnly; "
    "    var ok2 := DateOnly.TryParse(\"2024-03-15\", out d2); "
    "    if not ok2 then return 2; end; "
    "    if d2.Month <> 3 then return 3; end; "
    "    var t1: TimeOnly; "
    "    var ok3 := TimeOnly.TryParse(\"garbage\", out t1); "
    "    if ok3 then return 4; end; "
    "    var t2: TimeOnly; "
    "    var ok4 := TimeOnly.TryParse(\"13:45:30\", out t2); "
    "    if not ok4 then return 5; end; "
    "    if t2.Second <> 30 then return 6; end; "
    "    return 33; "
    "  end Main; "
    "end C;";

// (E) An UNCAUGHT malformed DateOnly.Parse terminates the process CLEANLY —
// _exit(70) (EX_SOFTWARE, the uncaught-exception boundary, §33.3 / #1609), not a
// SIGSEGV. On base 19b6566 the `unreachable` trap crashes (WIFSIGNALED →
// ran=false); with the fix the FormatException reaches the runtime's unhandled
// handler and the process exits 70 (WIFEXITED → ran=true).
const char* kUncaughtTerminatesCleanly =
    "module M; class C "
    "  public static method Main(): int "
    "    _ := DateOnly.Parse(\"garbage\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) Malformed DateOnly.Parse is CAUGHT as FormatException — the headline.
// stdlib §2.5, v0.197. runAot (PLAIN): pre-fix SIGSEGV → ran=false.
// =====================================================================
BEZEL_TEST(dateonly_timeonly_parse_uncatchable_2015, dateonly_malformed_parse_is_caught_stdlib_2_5) {
    JitResult r = runAot(kDateOnlyMalformedIsCaught);
    // runAot returns ran=false on a compile reject / crash / timeout. Pre-fix
    // the `unreachable` failure edge reads a wild pointer → SIGSEGV → ran=false.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // FormatException caught, clean exit
}

// =====================================================================
// (A2) Malformed TimeOnly.Parse is CAUGHT as FormatException — the headline.
// stdlib §2.5, v0.197. runAot (PLAIN): pre-fix SIGSEGV → ran=false.
// =====================================================================
BEZEL_TEST(dateonly_timeonly_parse_uncatchable_2015, timeonly_malformed_parse_is_caught_stdlib_2_5) {
    JitResult r = runAot(kTimeOnlyMalformedIsCaught);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // FormatException caught, clean exit
}

// =====================================================================
// (B) Every malformed DateOnly shape raises catchable FormatException; valid
// ones don't. stdlib §2.5, v0.197. runAotChecked (ASan/LSan) — leak-free.
// =====================================================================
BEZEL_TEST(dateonly_timeonly_parse_uncatchable_2015, dateonly_all_malformed_shapes_throw_stdlib_2_5) {
    JitResult r = runAotChecked(kDateOnlyAllMalformedShapes);
    BEZEL_EXPECT_TRUE(r.ran);        // pre-fix: wild-pointer read on the first case
    BEZEL_EXPECT_EQ(r.exitCode, 88);
}

// =====================================================================
// (B2) Every malformed TimeOnly shape raises; valid ones don't.
// stdlib §2.5, v0.197. runAotChecked (ASan/LSan) — leak-free.
// =====================================================================
BEZEL_TEST(dateonly_timeonly_parse_uncatchable_2015, timeonly_all_malformed_shapes_throw_stdlib_2_5) {
    JitResult r = runAotChecked(kTimeOnlyAllMalformedShapes);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 77);
}

// =====================================================================
// (C) Valid DateOnly `yyyy-MM-dd` and TimeOnly `HH:mm:ss` / `.fff` renderings
// still parse after the fix. stdlib §2.5. runAotChecked (ASan/LSan) — leak-free.
// =====================================================================
BEZEL_TEST(dateonly_timeonly_parse_uncatchable_2015, valid_renderings_still_parse_stdlib_2_5) {
    JitResult r = runAotChecked(kValidStillParses);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// =====================================================================
// (D) DateOnly.TryParse / TimeOnly.TryParse unaffected — malformed → false (no
// throw/crash), valid → true. stdlib §2.5. runAotChecked (ASan/LSan).
// =====================================================================
BEZEL_TEST(dateonly_timeonly_parse_uncatchable_2015, tryparse_returns_false_no_throw_stdlib_2_5) {
    JitResult r = runAotChecked(kTryParseUnaffected);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 33);
}

// =====================================================================
// (E) An UNCAUGHT malformed DateOnly.Parse terminates cleanly — _exit(70), not a
// SIGSEGV. stdlib §2.5 / §33.3. runAot (PLAIN): pre-fix SIGSEGV → ran=false.
// =====================================================================
BEZEL_TEST(dateonly_timeonly_parse_uncatchable_2015, uncaught_terminates_cleanly_stdlib_2_5) {
    JitResult r = runAot(kUncaughtTerminatesCleanly);
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: SIGSEGV → ran=false
    BEZEL_EXPECT_EQ(r.exitCode, 70);   // EX_SOFTWARE, uncaught-exception boundary
}
