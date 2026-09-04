// Regression tests for #3496 (spec §8.5 / §18.5 / §9.1, v0.287): a §18.5
// interpolation hole holding a NULLABLE record must dispatch the record's own
// `ToString(): string`, agreeing with the direct `.ToString()` call and with
// the non-nullable spelling of the SAME value.
//
// §8.5's first sentence makes the explicit call and the §18.5 hole ONE rule:
// "`ToString` is invoked implicitly inside string interpolation ([§18.5]) via
// virtual dispatch". The four spellings for one value agreed on three answers
// and disagreed with the fourth (the nullable interpolation hole):
//
//     p.ToString(), p: Pt?          ->  "PT!"     // declared member wins
//     Console.WriteLine($"{p}")     ->  "Pt(...)" // WRONG — drops the override
//
// ROOT CAUSE. The §18.5 nullable-value-type arm at `LlvmBackend_Exprs.cpp:510`
// recurses into `emitValueToString` with the inner record type (and the inner
// record value). `emitValueToString`'s record arm at
// `LlvmBackend_String.cpp:3167` knew only two defaults — the synthesized
// field-listing (when §9.2 admits the record) and the qualified type-name
// default (when §9.2 disqualifies it) — and never walked the class chain for
// a user-declared `ToString()` MethodSymbol. The interpolation-chain walk at
// `LlvmBackend_Exprs.cpp:815` is not reached for the nullable spelling because
// `LlvmBackend_Exprs.cpp:510` short-circuits the WHOLE chain with a
// tag-driven live/null split. So a record-with-user-ToString reached via
// `$"{p?}"` silently rendered the auto-property listing (or the qualified
// type name for an ineligible body record) when §8.5 requires the user
// override. The fix mirrors the interpolation-chain walk into
// `emitValueToString`'s record arm so every routing through that ONE renderer
// honours the user member identically.
//
// The five rows below pin:
//   1. the headlined defect — `$"{nullable_point}"` over a positional record
//      declaring a plain `public method ToString(): string` (no `override`);
//      pre-fix rendered the §9.1 field listing, post-fix renders the
//      override;
//   2. the non-nullable CONTROL — `$"{point}"` must keep printing the
//      override, never the field listing — pinning the fix did not regress
//      the working chain;
//   3. a nullable-int CONTROL — `$"{nullable_int}"` must still print the
//      integer ("3"), not "null";
//   4. a nullable-choice CONTROL — `$"{nullable_choice}"` must still print
//      the case name ("M3496.Red"), not "null";
//   5. a 500-iteration loop amplification for leaks — both the new
//      user-ToString dispatch and the unchanged null-rendering path stay
//      leak-clean under LSan/ASan.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

const char* kReproModule =
    "module M; "
    "record Pt(X: int, Y: int) "
    "  public method ToString(): string => \"PT!\"; "
    "end Pt; "
    "choice Color Red, Green, Blue; end Color; "
    "class C "
    "  public static method Main(): int "
    "    var p: Pt? := new Pt(7, 8); "
    "    Console.WriteLine(p.ToString()); "      // row A
    "    Console.WriteLine($\"[{p}]\"); "        // row B (the buggy site)
    "    var q: Pt := new Pt(7, 8); "
    "    Console.WriteLine(q.ToString()); "      // row C
    "    Console.WriteLine($\"[{q}]\"); "        // row D
    "    var ni: int? := 3; "
    "    Console.WriteLine($\"[{ni}]\"); "       // row 3 / nullable-int
    "    var nc: Color? := Color.Red; "
    "    Console.WriteLine($\"[{nc}]\"); "       // row 4 / nullable-choice
    "    return 0; "
    "  end Main; "
    "end C; ";

const char* kLoopModule =
    "module M; "
    "record Pt(X: int, Y: int) "
    "  public method ToString(): string => \"PT!\"; "
    "end Pt; "
    "class C "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 500 do "
    "      var p: Pt? := new Pt(i, i + 1); "
    "      var s: string := $\"[{p}]\"; "
    "      if s.Length <> 5 then return 1; end; "
    "      i := i + 1; "
    "    end while; "
    "    return 0; "
    "  end Main; "
    "end C; ";

}  // namespace

// Row 1 (the headlined defect) + rows 2 / 3 / 4 (controls) all run in one
// program so a single observable string pins the four spellings side-by-side.
// pre-fix: the second line (the buggy `$"[{p}]"` over the nullable `Pt?`)
// renders the synthesized §9.1 field listing `Pt(X: 7, Y: 8)` instead of the
// declared `PT!`; rows A, C, D print `PT!`. The expected text below is the
// post-fix shape — all four `Pt` rows agree on `PT!` per §8.5.
BEZEL_TEST(interp_nullable_record_tostring_3496,
           explicit_user_toString_honoured_on_nullable_hole_8_5_18_5) {
    BEZEL_EXPECT_EQ(runAotOut(kReproModule),
                    "PT!\n"
                    "[PT!]\n"
                    "PT!\n"
                    "[PT!]\n"
                    "[3]\n"
                    "[Color.Red]");
}

// Row 2 isolate: non-nullable hole still renders the user ToString,
// never the §9.1 field listing. pinned so the fix's added walk does not
// silently regress the previously-working interpolation chain.
BEZEL_TEST(interp_nullable_record_tostring_3496,
           non_nullable_hole_still_dispatches_user_toString_8_5) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "record Pt(X: int, Y: int) "
            "  public method ToString(): string => \"PT!\"; "
            "end Pt; "
            "class C "
            "  public static method Main(): int "
            "    var q: Pt := new Pt(7, 8); "
            "    Console.WriteLine($\"[{q}]\"); "
            "    return q.ToString().Length; "
            "  end Main; "
            "end C; "),
        "[PT!]");
}

// Row 5 / 500-iteration loop amplification: pin that the new walk does not
// leak the emitter's intermediate string across iterations. The loop composes
// 500 fresh nullable-record producers each requiring the new
// `emitMethodCall` path; LSan would surface any unbounded growth as a
// non-zero leak report. The return-1 path forces the assertion failure to be
// observable even if the per-iteration render diverges from the expected
// `[PT!]` shape.
BEZEL_TEST(interp_nullable_record_tostring_3496,
           nullable_record_user_toString_loop_no_leak_28_9) {
    BEZEL_EXPECT_EQ(runAotChecked(kLoopModule).exitCode, 0);
}
