// Codegen — `Guid.Parse` on a MALFORMED string must raise a CATCHABLE
// FormatException, not an uncatchable trap. Issue #2004 (HIGH — uncatchable
// crash where a catchable exception is specified).
//
// Spec: stdlib §2.6 (v0.197) —
//   `public static method Parse(s: string): Guid;`
//        // accepts the "D" and "N" renderings (§2.18); FormatException otherwise
// §2.18 defines exactly the "D" (36-char dashed) and "N" (32 hex, no hyphens)
// renderings; the strict-fault model (§2.18, error-model catchable-exceptions)
// makes any other input a *catchable* `FormatException`, like every other
// builtin guard. Spec revision v0.197.
//
// THE DEFECT (base 09ccb4e). LlvmBackend_Calls.cpp lowered the Guid.Parse
// failure edge to `CreateUnreachable()` — an uncatchable trap where the spec
// promises a catchable exception. `bz_guid_parse` returns 0 on a malformed
// string (and zeroes the out slot), so `Guid.Parse("not-a-guid")` branched to
// the `unreachable` block: a `catch FormatException` NEVER fired and execution
// fell through into undefined code, reading the zeroed/garbage Guid bytes as a
// pointer → wild read (SIGSEGV under plain codegen; ASan "unknown-crash"). Every
// malformed shape hit it: wrong length, a bad hex digit, the empty string, or a
// 36-char string with hyphens in the wrong places.
//
// THE FIX. Route the failure edge through the standard builtin-throw path —
// `emitGuardThrow(ok == 0, "FormatException")` — exactly the mechanism the
// sibling #1989 `Guid.ToString(format)` guard and the Decimal.Parse /
// convert-from-string guards already use. The malformed fault is now a
// first-class, catchable throw.
//
// SCOPE. `Guid.TryParse` (case D) was NEVER affected — it lowers via a distinct
// `bz_guid_try_parse` path that returns the bool and never traps; it must STILL
// return false (never throw) on malformed input. Valid "D"/"N" strings (case C)
// must still parse successfully after the fix.
//
// OBSERVABILITY. On base 09ccb4e each malformed-Parse case SIGSEGVs / ASan-
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

// (A) THE HEADLINE — a malformed literal fed to Guid.Parse raises a CATCHABLE
// FormatException, so the program reaches `return 0`. On base 09ccb4e the
// `unreachable` failure edge crashes (SIGSEGV) → runAot reports ran=false; the
// literal argument is immortal, so nothing leaks on either path. Run under plain
// runAot: the pre-fix crash manifests as ran=false, the strongest fail-without
// signal.
const char* kMalformedIsCaught =
    "module M; class C "
    "  public static method parseRaises(s: string): bool "
    "    var raised := false; "
    "    try "
    "      _ := Guid.Parse(s); "
    "    catch FormatException => raised := true; "
    "    end; "
    "    return raised; "
    "  end parseRaises; "
    "  public static method Main(): int "
    "    if not parseRaises(\"not-a-guid\") then return 1; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// (B) EVERY malformed shape raises a catchable FormatException, and the two
// valid renderings do NOT. Leak-free (immortal literals; a caught FormatException
// is freed by the catch machinery, per #1989 case D). On base the FIRST case
// crashes; with the fix returns 88.
const char* kAllMalformedShapes =
    "module M; class C "
    "  public static method parseRaises(s: string): bool "
    "    var raised := false; "
    "    try "
    "      _ := Guid.Parse(s); "
    "    catch FormatException => raised := true; "
    "    end; "
    "    return raised; "
    "  end parseRaises; "
    "  public static method Main(): int "
    "    if not parseRaises(\"not-a-guid\") then return 1; end; "                        // garbage
    "    if not parseRaises(\"\") then return 2; end; "                                  // empty
    "    if not parseRaises(\"6ba7b8109dad11d180b400c04fd430c\") then return 3; end; "   // 31 (N-1)
    "    if not parseRaises(\"6ba7b8109dad11d180b400c04fd430c8a\") then return 4; end; " // 33 (N+1)
    "    if not parseRaises(\"6ba7b810-9dad-11d1-80b4-00c04fd430c\") then return 5; end; "   // 35 (D-1)
    "    if not parseRaises(\"6ba7b810-9dad-11d1-80b4-00c04fd430c8a\") then return 6; end; " // 37 (D+1)
    "    if not parseRaises(\"6ba7b810-9dad-11d1-80b4-00c04fd430cg\") then return 7; end; "  // bad hex (D)
    "    if not parseRaises(\"6ba7b8109dad11d180b400c04fd430cg\") then return 8; end; "      // bad hex (N)
    "    if not parseRaises(\"6ba7b810x9dadx11d1x80b4x00c04fd430c8\") then return 9; end; "  // wrong hyphens
    "    if parseRaises(\"6ba7b810-9dad-11d1-80b4-00c04fd430c8\") then return 20; end; "     // valid D
    "    if parseRaises(\"6ba7b8109dad11d180b400c04fd430c8\") then return 21; end; "         // valid N
    "    if parseRaises(\"00000000-0000-0000-0000-000000000000\") then return 22; end; "    // Empty D
    "    return 88; "
    "  end Main; "
    "end C;";

// (C) The fix did not break VALID parsing — the "D", "N", and Empty renderings
// still parse to the right value. Leak-free literals. Returns 55.
const char* kValidStillParses =
    "module M; class C "
    "  public static method Main(): int "
    "    var g := Guid.Parse(\"6ba7b810-9dad-11d1-80b4-00c04fd430c8\"); "
    "    var byN := Guid.Parse(\"6ba7b8109dad11d180b400c04fd430c8\"); "
    "    if not byN.Equals(g) then return 1; end; "
    "    var e := Guid.Empty; "
    "    var ez := Guid.Parse(\"00000000-0000-0000-0000-000000000000\"); "
    "    if not ez.Equals(e) then return 2; end; "
    "    return 55; "
    "  end Main; "
    "end C;";

// (D) Guid.TryParse is UNAFFECTED — malformed input returns false (never throws,
// never crashes) and a valid string returns true with the parsed value. This
// path never trapped; the assertion pins that the #2004 fix left it alone.
// Returns 33.
const char* kTryParseUnaffected =
    "module M; class C "
    "  public static method Main(): int "
    "    var r1 := Guid.Empty; "
    "    var ok1 := Guid.TryParse(\"not-a-guid\", out r1); "
    "    if ok1 then return 1; end; "
    "    var r2 := Guid.Empty; "
    "    var ok2 := Guid.TryParse(\"6ba7b810-9dad-11d1-80b4-00c04fd430c8\", out r2); "
    "    if not ok2 then return 2; end; "
    "    var r3 := Guid.Empty; "
    "    var ok3 := Guid.TryParse(\"\", out r3); "
    "    if ok3 then return 3; end; "
    "    return 33; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) Malformed Guid.Parse is CAUGHT as FormatException — the headline.
// stdlib §2.6 / §2.18, v0.197. runAot (PLAIN): pre-fix SIGSEGV → ran=false.
// =====================================================================
BEZEL_TEST(guid_parse_uncatchable_2004, malformed_parse_is_caught_stdlib_2_6) {
    JitResult r = runAot(kMalformedIsCaught);
    // runAot returns ran=false on a compile reject / crash / timeout. Pre-fix
    // the `unreachable` failure edge reads a wild pointer → SIGSEGV → ran=false.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // FormatException caught, clean exit
}

// =====================================================================
// (B) Every malformed shape raises catchable FormatException; valid ones don't.
// stdlib §2.6 / §2.18, v0.197. runAotChecked (ASan/LSan) — leak-free.
// =====================================================================
BEZEL_TEST(guid_parse_uncatchable_2004, all_malformed_shapes_throw_stdlib_2_18) {
    JitResult r = runAotChecked(kAllMalformedShapes);
    BEZEL_EXPECT_TRUE(r.ran);        // pre-fix: wild-pointer read on the first case
    BEZEL_EXPECT_EQ(r.exitCode, 88);
}

// =====================================================================
// (C) Valid "D"/"N"/Empty renderings still parse after the fix. stdlib §2.6.
// runAotChecked (ASan/LSan) — leak-free.
// =====================================================================
BEZEL_TEST(guid_parse_uncatchable_2004, valid_renderings_still_parse_stdlib_2_6) {
    JitResult r = runAotChecked(kValidStillParses);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// =====================================================================
// (D) Guid.TryParse unaffected — malformed → false (no throw/crash),
// valid → true. stdlib §2.6. runAotChecked (ASan/LSan).
// =====================================================================
BEZEL_TEST(guid_parse_uncatchable_2004, tryparse_returns_false_no_throw_stdlib_2_6) {
    JitResult r = runAotChecked(kTryParseUnaffected);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 33);
}
