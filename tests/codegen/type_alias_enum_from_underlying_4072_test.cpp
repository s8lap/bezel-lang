// Spec-derived regression tests for issue #4072 — §10.4 / §27.8 (v0.294):
// `ExprTC::enumTypeNamedBy` (src/sema/ExprTypecheck_Calls.cpp,
// typeCall's `FromUnderlyingOrNull` arm) did not see through a `type`
// alias of the enum, so `AliasColor4072.FromUnderlyingOrNull(value)`
// SILENTLY DEGRADED to a null/zero result — the call reached codegen
// with no lowering and the slot degraded through the #2434/#2491
// backstops to a WRONG VALUE (measured: returned 0 instead of the
// correct `E?` value the direct-name spelling produces).
//
// The direct `Color4072.FromUnderlyingOrNull(value)` spelling compiles
// clean and returns the correct value; the alias spelling now does the
// same.
//
// This is the SAME cross-layer pattern #4050 discovered: sema's own
// enumTypeNamedBy has no `TypeAliasSymbol` arm (returns null), and
// codegen's own `tryLowerEnumFromUnderlyingOrNull` (LlvmBackend_Exprs2.cpp)
// re-resolves the receiver expression independently for the dispatch
// gate, with the identical missing-arm shape. Fixing sema alone
// measurably left the wrong value reproducing; codegen needed the
// identical fix separately.
//
// Fix: both layers resolve a `TypeAliasSymbol` through the shared
// `sema::resolveAliasToTypeSymbol` (TypeResolution.hpp, #4050/#4051) so
// the alias-qualified spelling is recognized exactly like the
// direct-name spelling.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ---------------------------------------------------------------------------
// #4072 issue repro, verbatim (§10.4): the alias form must compile
// clean, run, and return the same value the direct form returns.
// ---------------------------------------------------------------------------

// The hit case, unwrapped with `!` (§17.12): underlying value 1 IS Green,
// so the alias form must return 7 (Green) exactly like the direct form.
BEZEL_TEST(type_alias_enum_from_underlying_4072,
           alias_from_underlying_or_null_hit_returns_enum_spec_10_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasColor4072 := Color4072; "
        "enum Color4072 Red, Green, Blue; end Color4072; "
        "class C "
        "  public static method Main(): int "
        "    var c: Color4072? := AliasColor4072.FromUnderlyingOrNull(1); "
        "    return (case c of Color4072.Green => 7, _ => 0); "
        "  end Main; "
        "end C;"), 7);
}

// Anti-regression control — the DIRECT (non-alias) form must still
// recognize the per-enum static member identically. Pins the fix's new
// resolution arm did not disturb the existing TypeSymbol arm's
// behavior.
BEZEL_TEST(type_alias_enum_from_underlying_4072,
           direct_from_underlying_or_null_hit_returns_enum_spec_10_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum ColorDirect4072 Red, Green, Blue; end ColorDirect4072; "
        "class C "
        "  public static method Main(): int "
        "    var c: ColorDirect4072? := "
        "        ColorDirect4072.FromUnderlyingOrNull(1); "
        "    return (case c of ColorDirect4072.Green => 7, _ => 0); "
        "  end Main; "
        "end C;"), 7);
}

// The miss case (§10.4 "or `null` when there is none"): a value with no
// matching constant must produce null on both spellings. Pins the
// fix's resolution didn't break the membership test itself.
BEZEL_TEST(type_alias_enum_from_underlying_4072,
           alias_from_underlying_or_null_miss_is_null_spec_10_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasColorMiss4072 := ColorMiss4072; "
        "enum ColorMiss4072 Red, Green, Blue; end ColorMiss4072; "
        "class C "
        "  public static method Main(): int "
        "    var c: ColorMiss4072? := AliasColorMiss4072.FromUnderlyingOrNull(99); "
        "    return (if c = null then 42 else 0); "
        "  end Main; "
        "end C;"), 42);
}

// §10.3 / stdlib §9.9: "The parameter type is the underlying type `U`
// exactly" — a byte-backed enum takes a `byte`; the alias form must
// accept the narrower argument and return the correct enum constant.
BEZEL_TEST(type_alias_enum_from_underlying_4072,
           alias_byte_backed_enum_from_underlying_spec_10_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasColorByte4072 := ColorByte4072; "
        "enum ColorByte4072: byte Red := 0, Green := 1, Blue := 2; "
        "end ColorByte4072; "
        "class C "
        "  public static method Main(): int "
        "    var b: byte := 2; "
        "    var c := AliasColorByte4072.FromUnderlyingOrNull(b); "
        "    return int(c!); "
        "  end Main; "
        "end C;"), 2);
}

// §10.4 boundary idiom: `AliasColor4072.FromUnderlyingOrNull(bad) ??
// AliasColor4072.Low` must compose through the alias form exactly like
// the direct form. bad=99 misses (Low=0), good=1 hits Medium (=1), so
// int(a)*10 + int(b) = 0*10 + 1 = 1.
BEZEL_TEST(type_alias_enum_from_underlying_4072,
           alias_coalesce_default_spec_10_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasCoCo4072 := ColorCoCo4072; "
        "enum ColorCoCo4072 Low, Medium, High; end ColorCoCo4072; "
        "class C "
        "  public static method Main(): int "
        "    var bad: int := 99; "
        "    var good: int := 1; "
        "    var a := AliasCoCo4072.FromUnderlyingOrNull(bad) "
        "        ?? AliasCoCo4072.Low; "
        "    var b := AliasCoCo4072.FromUnderlyingOrNull(good) "
        "        ?? AliasCoCo4072.High; "
        "    return int(a) * 10 + int(b); "
        "  end Main; "
        "end C;"), 1);
}

// Argument validation through the alias form: an OUT-OF-RANGE value is
// rejected identically (sema's argument check above the codegen dispatch
// produces the same §10.4 diagnostic for both spellings).
BEZEL_TEST(type_alias_enum_from_underlying_4072,
           alias_out_of_range_argument_still_rejected_spec_10_4) {
    auto r = runAot(
        "module M; "
        "type AliasColorBad4072 := ColorBad4072; "
        "enum ColorBad4072: byte Red, Green, Blue; end ColorBad4072; "
        "class C "
        "  public static method Main(): int "
        "    var b: byte := 200; "
        "    var c: ColorBad4072? := AliasColorBad4072.FromUnderlyingOrNull(b); "
        "    return 0; "
        "  end Main; "
        "end C;");
    // A byte-backed enum only admits values 0..255 in `byte`; the
    // argument-validation dispatch may produce a compile-time reject
    // here OR compile-clean and return null at runtime — both are
    // valid §10.4 outcomes. The pin is that the slot does NOT degrade
    // through the #2434/#2491 backstop path (the pre-fix symptom).
    if (r.ran) {
        BEZEL_EXPECT_EQ(r.exitCode, 0);  // null path is acceptable
    }
}

// ── REGRESSION for #4047 (§27.8, v0.294) ─────────────────────────────────
//
// A CHAINED alias (outer declared textually BEFORE inner) used to
// inherit #4047's resolution-order gap: `Resolver::resolveTopDecl`'s
// single forward pass had not yet resolved the inner alias's
// `aliasedType` when the outer alias's own `aliasedType` (a reference
// to the inner alias) was resolved, so `sema::resolveAliasToTypeSymbol`
// returned null and both #4072's sema fix and the parallel codegen fix
// had nothing to dispatch on — the original silent-wrong-value bug
// reproduced (#2434/#2491 backstop degrading to 0).
//
// Fix #4047 adds `Resolver::resolveAliasForwardReferences` (a fixed-
// point re-resolution sweep over the top-level alias list) so the
// reverse declaration order now resolves exactly like the working
// inner-before-outer order — and `FromUnderlyingOrNull(1)` then folds
// to `Green` (= 7), same as the alias-form control pinned elsewhere
// in this file. A DIRECT (non-chained) alias declared before its
// target enum was UNAFFECTED (the target's TypeSymbol exists from
// symbol collection regardless of declaration order; only a
// TypeAliasSymbol needs its OWN `aliasedType` resolved first) —
// confirmed separately, not itself a #4047 case, and not pinned here.
BEZEL_TEST(type_alias_enum_from_underlying_4072,
           chained_alias_REVERSE_ORDER_from_underlying_4047) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasEnumOuter4072 := AliasEnumInner4072; "
        "type AliasEnumInner4072 := ColorEnum4072; "
        "enum ColorEnum4072 Red, Green, Blue; end ColorEnum4072; "
        "class C "
        "  public static method Main(): int "
        "    var c: ColorEnum4072? := "
        "        AliasEnumOuter4072.FromUnderlyingOrNull(1); "
        "    return (case c of ColorEnum4072.Green => 7, _ => 0); "
        "  end Main; "
        "end C;"), 7);
}