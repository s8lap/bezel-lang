// Codegen — `Guid.ToString(format)` overload dispatch AND `Guid.Parse`
// accepting the "N" rendering. Issue #1989.
//
// Spec: stdlib §2.6 (v0.195) —
//   `public static method Parse(s: string): Guid;`
//        // accepts the "D" and "N" renderings (§2.18); FormatException otherwise
//   `Guid.ToString(format: string): string` — the format overload.
// stdlib §2.18 Guid format vocabulary: exactly "D" (canonical 36-char dashed,
// = the no-format rendering) and "N" (32 lowercase hex, no hyphens); any other
// specifier raises FormatException (strict-fault). Spec revision v0.197.
//
// TWO DEFECTS closed here, both surfaced by the idiomatic ToString/Parse
// round-trip. (The #1989 review's "block-split" hypothesis was a
// mis-attribution: `Guid.Parse(g.ToString("D"))` never crashes — the 36-char
// string parses fine — only the *"N"* round-trip did, and the cause was the
// parser, not the FormatException block split.)
//
//   (1) THE ToString(format) OVERLOAD DID NOT EXIST. `var s := g.ToString("N")`
//       compiled but the one-arg overload fell through to a generic path that
//       returned a garbage value (not a valid bz.string); the next use aborted.
//       FIX: lower `g.ToString(format)` (LlvmBackend_Calls.cpp Guid block) to a
//       new runtime bz_guid_to_string_fmt(g, fmt) — "D"→bz_guid_to_string,
//       "N"→bz_guid_to_string_n, else nullptr → emitGuardThrow("FormatException").
//       isPlusOneStringProducer (LlvmBackend_Setup.cpp) recognizes the overload
//       as a +1 producer, exactly as it does the no-arg #1982 form.
//
//   (2) Guid.Parse REJECTED THE "N" FORM — the real memory-safety crash. The
//       runtime guid_parse_str only accepted the 36-char "D" form (len != 36 →
//       fail); a 32-char "N" string (whether a literal or the output of
//       ToString("N")) failed, and the codegen Parse path traps (`unreachable`)
//       on failure — so `Guid.Parse(<32-char N string>)` fell through the trap
//       into undefined code and read a wild pointer (ASan "unknown-crash /
//       SEGV"; a bare SIGSEGV without ASan). FIX: guid_parse_str now accepts
//       BOTH the 36-char "D" and the 32-char "N" renderings per §2.6.
//
// OBSERVABILITY. Each case below FAILS on a build missing EITHER fix (a garbage
// value from defect 1, or a wild-pointer read / SEGV from defect 2) and PASSES
// with both — verified by temporarily reverting the guid_parse_str "N" branch
// (SEGV/exit 99 under ASan, SIGSEGV under plain codegen) and rebuilding.
//
// HARNESS CHOICE. The leak-free cases (A–D: value comparisons, bound-then-
// consume, and Parse of an IMMORTAL string LITERAL) run under runAotChecked
// (AddressSanitizer/LeakSanitizer). The exact reviewer-mandated composition
// (E: Guid.Parse(g.ToString("D")) with the ToString RESULT fed to Parse) is run
// under runAot (plain) instead, because passing a heap string PRODUCER to the
// borrowing Guid.Parse leaks it — a PRE-EXISTING, #1989-independent defect that
// reproduces identically with the no-arg form (`var s := g.ToString(); ...
// Guid.Parse(s)` leaks 69 bytes on origin). Under plain runAot the #1989 crash
// still manifests (SIGSEGV) so the fail-without/pass-with contract holds, while
// the orthogonal leak — flagged separately — does not mask it. Case (C) gives
// the after-churn composition its ASan-clean guarantee using the leak-free
// literal-N form.

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

// (A) ToString(format) VALUE + width correctness — the "N" (32 hex, no hyphens)
// and "D" (36-char dashed) renderings on a parsed Guid and on Guid.Empty, plus
// the no-format == "D" invariant (§2.18). Each format result is consumed by a
// comparison or `.Length` (leak-free: no producer is passed to a call). A
// garbage value (defect 1) fires ASan. Returns 77 iff every rendering is exact.
const char* kFormatValues =
    "module M; class C "
    "  public static method Main(): int "
    "    var g: Guid := Guid.Parse(\"6ba7b810-9dad-11d1-80b4-00c04fd430c8\"); "
    "    var e: Guid := Guid.Empty; "
    "    if g.ToString(\"N\") <> \"6ba7b8109dad11d180b400c04fd430c8\" then return 1; end; "
    "    if g.ToString(\"D\") <> \"6ba7b810-9dad-11d1-80b4-00c04fd430c8\" then return 2; end; "
    "    if e.ToString(\"N\") <> \"00000000000000000000000000000000\" then return 3; end; "
    "    if e.ToString(\"D\") <> \"00000000-0000-0000-0000-000000000000\" then return 4; end; "
    "    if g.ToString(\"N\").Length <> 32 then return 5; end; "
    "    if g.ToString(\"D\").Length <> 36 then return 6; end; "
    "    if g.ToString() <> g.ToString(\"D\") then return 7; end; "
    "    return 77; "
    "  end Main; "
    "end C;";

// (B) Guid.Parse ACCEPTS BOTH renderings — the parser fix (defect 2), exercised
// leak-free with IMMORTAL string literals (the "N" 32-char and "D" 36-char forms
// of the same Guid). Pre-fix the 32-char "N" literal fails to parse and the
// codegen Parse trap reads a wild pointer (ASan SEGV). Returns 55 iff both
// literal forms parse to the original Guid.
const char* kParseAcceptsBothLiterals =
    "module M; class C "
    "  public static method Main(): int "
    "    var g: Guid := Guid.Parse(\"6ba7b810-9dad-11d1-80b4-00c04fd430c8\"); "
    "    var byN := Guid.Parse(\"6ba7b8109dad11d180b400c04fd430c8\"); "     // "N"
    "    if not byN.Equals(g) then return 1; end; "
    "    var byD := Guid.Parse(\"6ba7b810-9dad-11d1-80b4-00c04fd430c8\"); "  // "D"
    "    if not byD.Equals(g) then return 2; end; "
    "    var e: Guid := Guid.Empty; "
    "    var ezN := Guid.Parse(\"00000000000000000000000000000000\"); "      // Empty "N"
    "    if not ezN.Equals(e) then return 3; end; "
    "    return 55; "
    "  end Main; "
    "end C;";

// (C) The COMPOSITION after FormatException churn, ASan-clean. Several bad-format
// try/catch blocks churn the heap FIRST, THEN the round-trips run — the "D" and
// "N" forms rendered by ToString(format) are re-parsed via the leak-free literal
// path (bind → compare, then Parse the LITERAL of the same rendering) so the
// whole case is LeakSanitizer-clean while still exercising the parser after
// churn. Pre-fix: the "N" literal Parse reads a wild pointer after the churn.
// Returns 0 on full success; a small code names the first failed assertion.
const char* kCompositionAfterChurnLeakFree =
    "module M; class C "
    "  public static method badRaises(fmt: string): bool "
    "    var g: Guid := Guid.Empty; "
    "    var raised := false; "
    "    try "
    "      var s := g.ToString(fmt); "
    "      _ := s.Length; "
    "    catch FormatException => raised := true; "
    "    end; "
    "    return raised; "
    "  end badRaises; "
    "  public static method Main(): int "
    "    var g: Guid := Guid.Parse(\"6ba7b810-9dad-11d1-80b4-00c04fd430c8\"); "
    "    if not badRaises(\"n\") then return 10; end; "        // case-sensitive
    "    if not badRaises(\"d\") then return 11; end; "
    "    if not badRaises(\"\") then return 12; end; "         // empty
    "    if not badRaises(\"NN\") then return 13; end; "       // multi-char
    "    if not badRaises(\"X\") then return 14; end; "        // out of vocab
    "    if badRaises(\"N\") then return 15; end; "            // good must not raise
    "    if badRaises(\"D\") then return 16; end; "
    "    var renderedN := g.ToString(\"N\"); "
    "    if renderedN <> \"6ba7b8109dad11d180b400c04fd430c8\" then return 17; end; "
    "    var renderedD := g.ToString(\"D\"); "
    "    if renderedD <> \"6ba7b810-9dad-11d1-80b4-00c04fd430c8\" then return 18; end; "
    "    var backN := Guid.Parse(\"6ba7b8109dad11d180b400c04fd430c8\"); "
    "    if not backN.Equals(g) then return 19; end; "
    "    var backD := Guid.Parse(\"6ba7b810-9dad-11d1-80b4-00c04fd430c8\"); "
    "    if not backD.Equals(g) then return 20; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// (D) Bad specifiers raise a CATCHABLE FormatException (§2.18 strict-fault):
// lowercase "n"/"d" (case-sensitive), empty "", multi-char "NN", and an
// out-of-vocabulary "X"; a runtime (non-literal) format string exercises the
// same dispatch. Good "N"/"D" must NOT raise. Without the overload (defect 1) a
// bad specifier returned garbage instead of throwing, so `raised` stayed false.
// Leak-free: the good-path string is bound and consumed by `.Length`, then
// released at scope end (isPlusOneStringProducer). Returns 88 iff all correct.
const char* kBadSpecifiersThrow =
    "module M; class C "
    "  public static method fmtRaises(g: Guid, fmt: string): bool "
    "    var raised := false; "
    "    try "
    "      var s := g.ToString(fmt); "
    "      _ := s.Length; "
    "    catch FormatException => raised := true; "
    "    end; "
    "    return raised; "
    "  end fmtRaises; "
    "  public static method Main(): int "
    "    var g: Guid := Guid.Parse(\"6ba7b810-9dad-11d1-80b4-00c04fd430c8\"); "
    "    if not fmtRaises(g, \"n\") then return 1; end; "
    "    if not fmtRaises(g, \"d\") then return 2; end; "
    "    if not fmtRaises(g, \"\") then return 3; end; "
    "    if not fmtRaises(g, \"NN\") then return 4; end; "
    "    if not fmtRaises(g, \"X\") then return 5; end; "
    "    if fmtRaises(g, \"N\") then return 6; end; "
    "    if fmtRaises(g, \"D\") then return 7; end; "
    "    return 88; "
    "  end Main; "
    "end C;";

// (E) The EXACT reviewer-mandated composition — `Guid.Parse(g.ToString("D"))`
// AND `Guid.Parse(g.ToString("N"))` with the ToString RESULT fed to Parse,
// after FormatException churn. Run under runAot (PLAIN) because feeding a heap
// string producer to the borrowing Guid.Parse leaks it (the PRE-EXISTING,
// #1989-independent defect — see the header). The #1989 crash still manifests
// here as a SIGSEGV without the fix (the "N" round-trip's Parse trap), so the
// fail-without/pass-with contract holds; the orthogonal leak is invisible to
// plain runAot and does not mask it. Returns 0 on full success.
const char* kCompositionToStringFed =
    "module M; class C "
    "  public static method badRaises(fmt: string): bool "
    "    var g: Guid := Guid.Empty; "
    "    var raised := false; "
    "    try "
    "      var s := g.ToString(fmt); "
    "      _ := s.Length; "
    "    catch FormatException => raised := true; "
    "    end; "
    "    return raised; "
    "  end badRaises; "
    "  public static method Main(): int "
    "    var g: Guid := Guid.Parse(\"6ba7b810-9dad-11d1-80b4-00c04fd430c8\"); "
    "    if not badRaises(\"n\") then return 10; end; "
    "    if not badRaises(\"\") then return 11; end; "
    "    if not badRaises(\"NN\") then return 12; end; "
    "    if not badRaises(\"X\") then return 13; end; "
    "    var back := Guid.Parse(g.ToString(\"D\")); "     // ToString RESULT → Parse
    "    if not back.Equals(g) then return 20; end; "
    "    var backN := Guid.Parse(g.ToString(\"N\")); "    // the crash case pre-fix
    "    if not backN.Equals(g) then return 21; end; "
    "    var interpN := $\"{g:N}\"; "
    "    if interpN <> g.ToString(\"N\") then return 22; end; "
    "    if g.ToString() <> g.ToString(\"D\") then return 23; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) ToString(format) value + width — "N"/"D" on a parsed and the Empty Guid.
// stdlib §2.6 / §2.18. runAotChecked (ASan/LSan) — leak-free.
// =====================================================================
BEZEL_TEST(guid_tostring_format_1989,
           format_values_asan_clean_stdlib_2_6) {
    JitResult r = runAotChecked(kFormatValues);
    BEZEL_EXPECT_TRUE(r.ran);        // compiled + no garbage-value use
    BEZEL_EXPECT_EQ(r.exitCode, 77);
}

// =====================================================================
// (B) Guid.Parse accepts BOTH "N" and "D" renderings (literals). stdlib §2.6.
// runAotChecked (ASan) — the "N" literal Parse SEGVs pre-fix.
// =====================================================================
BEZEL_TEST(guid_tostring_format_1989,
           parse_accepts_n_and_d_literals_asan_clean_stdlib_2_6) {
    JitResult r = runAotChecked(kParseAcceptsBothLiterals);
    BEZEL_EXPECT_TRUE(r.ran);        // pre-fix: wild-pointer read in the Parse trap
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// =====================================================================
// (C) Composition after FormatException churn — ASan-clean (leak-free literal
// re-parse). stdlib §2.6 / §2.18. runAotChecked (ASan/LSan).
// =====================================================================
BEZEL_TEST(guid_tostring_format_1989,
           composition_after_churn_asan_clean_stdlib_2_6) {
    JitResult r = runAotChecked(kCompositionAfterChurnLeakFree);
    BEZEL_EXPECT_TRUE(r.ran);        // pre-fix: wild-pointer read after churn
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // every assertion passed
}

// =====================================================================
// (D) Bad specifiers raise catchable FormatException. stdlib §2.18 strict-fault.
// runAotChecked (ASan/LSan) — leak-free.
// =====================================================================
BEZEL_TEST(guid_tostring_format_1989,
           bad_specifiers_throw_format_exception_stdlib_2_18) {
    JitResult r = runAotChecked(kBadSpecifiersThrow);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 88);
}

// =====================================================================
// (E) The exact reviewer composition — Guid.Parse(g.ToString("D")) / via "N"
// with the ToString RESULT fed to Parse, after churn. runAot (PLAIN): the
// #1989 crash still SIGSEGVs pre-fix; the orthogonal producer→Parse leak is not
// under test here. stdlib §2.6 / §2.18.
// =====================================================================
BEZEL_TEST(guid_tostring_format_1989,
           composition_tostring_fed_parse_no_crash_stdlib_2_6) {
    JitResult r = runAot(kCompositionToStringFed);
    // runAot returns ran=false only on a compile reject / crash / timeout; a
    // successful program has ran=true and exitCode set. Pre-fix the "N"
    // round-trip SIGSEGVs → ran=false.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // no crash, every assertion passed
}
