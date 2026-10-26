// Codegen — `DateTime.Parse` on a MALFORMED string must raise a CATCHABLE
// FormatException, not an uncatchable trap. Issue #2009 (HIGH — uncatchable
// crash where a catchable exception is specified). Near-exact mirror of #2004
// (Guid.Parse); the DateTime.Parse lowering's own comment said "same pattern as
// Guid.Parse."
//
// Spec: stdlib §2.5 (v0.197) —
//   `public static method Parse(s: string): DateTime;`
//   Parse and range contracts (v0.195): "DateTime.Parse accepts exactly the
//   type's own rendered forms — the round-trip form `yyyy-MM-ddTHH:mm:ss`,
//   optionally extended with a fractional-seconds tail `.fff`, and the
//   date-only form `yyyy-MM-dd` (time components zero) ... any other input
//   raises FormatException, the DateOnly.Parse stance."
// The strict-fault model (error-model catchable-exceptions) makes any other
// input a *catchable* FormatException, like every other builtin guard. Spec
// revision v0.197.
//
// THE DEFECT (base 9d2f00a). LlvmBackend_Calls.cpp lowered the DateTime.Parse
// failure edge to `CreateUnreachable()` — an uncatchable trap where the spec
// promises a catchable exception. `bz_datetime_try_parse` returns 0 on a
// malformed string, so `DateTime.Parse("garbage")` branched to the `unreachable`
// block: a `catch FormatException` NEVER fired and execution fell through into
// undefined code → wild-pointer read (SIGSEGV under plain codegen; ASan
// "unknown-crash"). Every malformed shape hit it: garbage, empty, an
// out-of-range field, or a well-shaped prefix with trailing junk.
//
// THE FIX. Route the failure edge through the standard builtin-throw path —
// `emitGuardThrow(ok == 0, "FormatException")` — exactly the mechanism the
// sibling #2004 `Guid.Parse` guard and the Decimal.Parse / convert-from-string
// guards already use. The malformed fault is now a first-class, catchable throw.
//
// SCOPE. `DateTime.TryParse` (case D) was NEVER affected — it lowers via a
// distinct path that returns the bool and never traps; it must STILL return
// false (never throw) on malformed input. Valid round-trip / `.fff` / date-only
// strings (case C) must still parse successfully after the fix.
//
// OBSERVABILITY. On base 9d2f00a each malformed-Parse case SIGSEGVs / ASan-
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

// (A) THE HEADLINE — a malformed literal fed to DateTime.Parse raises a
// CATCHABLE FormatException, so the program reaches `return 0`. On base 9d2f00a
// the `unreachable` failure edge crashes (SIGSEGV) → runAot reports ran=false;
// the literal argument is immortal, so nothing leaks on either path. Run under
// plain runAot: the pre-fix crash manifests as ran=false, the strongest
// fail-without signal.
const char* kMalformedIsCaught =
    "module M; class C "
    "  public static method parseRaises(s: string): bool "
    "    var raised := false; "
    "    try "
    "      _ := DateTime.Parse(s); "
    "    catch FormatException => raised := true; "
    "    end; "
    "    return raised; "
    "  end parseRaises; "
    "  public static method Main(): int "
    "    if not parseRaises(\"garbage\") then return 1; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// (B) EVERY malformed shape raises a catchable FormatException, and the valid
// renderings do NOT. Leak-free (immortal literals; a caught FormatException is
// freed by the catch machinery, per #2004 case B). On base the FIRST case
// crashes; with the fix returns 88.
const char* kAllMalformedShapes =
    "module M; class C "
    "  public static method parseRaises(s: string): bool "
    "    var raised := false; "
    "    try "
    "      _ := DateTime.Parse(s); "
    "    catch FormatException => raised := true; "
    "    end; "
    "    return raised; "
    "  end parseRaises; "
    "  public static method Main(): int "
    "    if not parseRaises(\"garbage\") then return 1; end; "                       // non-numeric junk
    "    if not parseRaises(\"\") then return 2; end; "                              // empty
    "    if not parseRaises(\"2024-13-01\") then return 3; end; "                    // month 13 out of range
    "    if not parseRaises(\"2024-01-00\") then return 4; end; "                    // day 0 out of range
    "    if not parseRaises(\"2024-01-15T25:00:00\") then return 5; end; "           // hour 25 out of range
    "    if not parseRaises(\"2024-01-15T13:45:60\") then return 6; end; "           // second 60 out of range
    "    if not parseRaises(\"2024-01-15xtrailing\") then return 7; end; "           // valid prefix + junk
    "    if not parseRaises(\"2024-01-15T13:45:30.50\") then return 8; end; "        // 2-digit .ff (needs 3)
    "    if not parseRaises(\"2024/01/15\") then return 9; end; "                    // wrong separators
    "    if parseRaises(\"2024-01-15T13:45:30\") then return 20; end; "              // valid round-trip
    "    if parseRaises(\"2024-01-15T13:45:30.500\") then return 21; end; "          // valid .fff
    "    if parseRaises(\"2024-01-15\") then return 22; end; "                       // valid date-only
    "    return 88; "
    "  end Main; "
    "end C;";

// (C) The fix did not break VALID parsing — the round-trip, `.fff`, and
// date-only renderings still parse to the right value (fields read back exact;
// date-only zeroes the time-of-day, per §2.5). Leak-free literals. Returns 55.
const char* kValidStillParses =
    "module M; class C "
    "  public static method Main(): int "
    "    var rt := DateTime.Parse(\"2024-03-15T13:45:30\"); "
    "    if rt.Year <> 2024 then return 1; end; "
    "    if rt.Month <> 3 then return 2; end; "
    "    if rt.Day <> 15 then return 3; end; "
    "    if rt.Hour <> 13 then return 4; end; "
    "    if rt.Minute <> 45 then return 5; end; "
    "    if rt.Second <> 30 then return 6; end; "
    "    var frac := DateTime.Parse(\"2024-03-15T13:45:30.500\"); "
    "    if frac.Millisecond <> 500 then return 7; end; "
    "    var d := DateTime.Parse(\"2024-03-15\"); "
    "    if d.Year <> 2024 then return 8; end; "
    "    if d.Month <> 3 then return 9; end; "
    "    if d.Day <> 15 then return 10; end; "
    "    if d.Hour <> 0 then return 11; end; "         // date-only → time components zero
    "    if d.Minute <> 0 then return 12; end; "
    "    if d.Second <> 0 then return 13; end; "
    "    return 55; "
    "  end Main; "
    "end C;";

// (D) DateTime.TryParse is UNAFFECTED — malformed input returns false (never
// throws, never crashes) and a valid string returns true with the parsed value.
// This path never trapped; the assertion pins that the #2009 fix left it alone.
// Returns 33.
const char* kTryParseUnaffected =
    "module M; class C "
    "  public static method Main(): int "
    "    var r1 := DateTime.MinValue; "
    "    var ok1 := DateTime.TryParse(\"garbage\", out r1); "
    "    if ok1 then return 1; end; "
    "    var r2 := DateTime.MinValue; "
    "    var ok2 := DateTime.TryParse(\"2024-03-15T13:45:30\", out r2); "
    "    if not ok2 then return 2; end; "
    "    if r2.Year <> 2024 then return 3; end; "
    "    var r3 := DateTime.MinValue; "
    "    var ok3 := DateTime.TryParse(\"\", out r3); "
    "    if ok3 then return 4; end; "
    "    return 33; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) Malformed DateTime.Parse is CAUGHT as FormatException — the headline.
// stdlib §2.5, v0.197. runAot (PLAIN): pre-fix SIGSEGV → ran=false.
// =====================================================================
BEZEL_TEST(datetime_parse_uncatchable_2009, malformed_parse_is_caught_stdlib_2_5) {
    JitResult r = runAot(kMalformedIsCaught);
    // runAot returns ran=false on a compile reject / crash / timeout. Pre-fix
    // the `unreachable` failure edge reads a wild pointer → SIGSEGV → ran=false.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // FormatException caught, clean exit
}

// =====================================================================
// (B) Every malformed shape raises catchable FormatException; valid ones don't.
// stdlib §2.5, v0.197. runAotChecked (ASan/LSan) — leak-free.
// =====================================================================
BEZEL_TEST(datetime_parse_uncatchable_2009, all_malformed_shapes_throw_stdlib_2_5) {
    JitResult r = runAotChecked(kAllMalformedShapes);
    BEZEL_EXPECT_TRUE(r.ran);        // pre-fix: wild-pointer read on the first case
    BEZEL_EXPECT_EQ(r.exitCode, 88);
}

// =====================================================================
// (C) Valid round-trip / `.fff` / date-only renderings still parse after the
// fix. stdlib §2.5. runAotChecked (ASan/LSan) — leak-free.
// =====================================================================
BEZEL_TEST(datetime_parse_uncatchable_2009, valid_renderings_still_parse_stdlib_2_5) {
    JitResult r = runAotChecked(kValidStillParses);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// =====================================================================
// (D) DateTime.TryParse unaffected — malformed → false (no throw/crash),
// valid → true. stdlib §2.5. runAotChecked (ASan/LSan).
// =====================================================================
BEZEL_TEST(datetime_parse_uncatchable_2009, tryparse_returns_false_no_throw_stdlib_2_5) {
    JitResult r = runAotChecked(kTryParseUnaffected);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 33);
}
