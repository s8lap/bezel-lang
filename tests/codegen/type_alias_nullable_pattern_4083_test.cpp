// Spec-derived tests for fix #4083 — §19.3 / §19.5 / §27.8 (v0.294): a `type`
// alias whose underlying type is a NULLABLE PRIMITIVE (`type X := int?;`),
// used as a bare, NON-alt-list NamedPattern on a nullable-VALUE scrutinee
// (`case x of X => …`, where `x: int?`), used to crash the LLVM IR verifier
// with "Basic Block … does not have terminator" + the #2491 backstop —
// pre-existing on `eae547e` and surviving #4076 (which only added a
// `TypeAliasSymbol` arm in the sibling record/enum / class/interface name-
// resolution loops and correctly continues for a `resolveAliasToTypeSymbol`
// result whose underlying `TypeRef::Kind` is `Primitive`, since that helper's
// contract — `TypeResolution_Names.cpp:2852-2858` — rejects non-`Named` kinds
// outright).
//
// Root cause is the same shape fix #4076 closed for class/interface/record/
// enum aliases — codegen's pattern matching independently re-resolves a
// written name against `ctx_.symbols()` rather than consuming sema's
// resolution — applied to the *nullable-primitive* aliased-kind case. The
// isNullableVal branch's bare-NamedPattern handler (LlvmBackend_Exprs2.cpp)
// had only TWO arms before this fix:
//   (1) enum-constant match → emit equality + AND with hasVal
//   (2) bare-name match (only ever fires for a `Named`-kind scrutinee with a
//       registered `declaration`, so primitive `scrutTy` is skipped)
// neither of which accepts a `TypeAliasSymbol`, so a nullable-primitive
// alias fell through to the bare `return nullptr` (deferred). Deferred +
// a single-pattern arm left `case.end` unterminated → IR-verify crash.
//
// The fix adds a primitive-aware alias arm BETWEEN the bare-name check and
// the deferred `return nullptr`: it walks `ctx_.symbols()` for a
// `TypeAliasSymbol` whose name matches the pattern, resolves
// `aliasedType`'s underlying `TypeRef`, and — when both the alias and the
// scrutinee are the SAME nullable primitive — returns `hasVal` exactly
// when the value is present (matching the `TypePattern` "case x of int? =>
// …" handled at L11115). For an alias whose underlying kind is anything
// ELSE (function, InlineArray, a non-matching primitive, …), the fix emits
// the sibling #1847 §19.3 diagnostic and ORs a never-match (`getInt1(false)`)
// so `case.end` stays terminated — the same diagnostic-AND-never-match
// shape #2227 uses for the adjacent ConstantPattern type-mismatch case.
// An alias that does not exist at all keeps the existing `return nullptr`
// fall-through (no in-scope shape today reaches that branch through
// isNullableVal besides primitives this branch now handles).
//
// Anti-regression: the DIRECT (non-alias) `int?` pattern (`TypePattern`,
// not `NamedPattern`) must keep behaving identically — pins that the new
// alias arm does not disturb the pre-existing `TypePattern`-on-`int?`
// path that the existing `case_nullable_value_type_pattern_1128_test.cpp`
// already covers.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
constexpr const char* kBackstopMsg = "not supported by the backend";
}  // namespace

// #4083 issue repro, verbatim (§19.3 / §19.5 / §27.8): a bare `type` alias
// of a nullable primitive used as a single, non-alt-list NamedPattern on
// a nullable-VALUE scrutinee must MATCH and compute the correct value
// (7) — exactly like the direct `int?` `TypePattern` form already does,
// not crash. Pre-fix: LLVM IR verifier crash ("Basic Block … does not
// have terminator" + #2491 backstop); post-fix: clean compile, matches
// when value present, value 7.
BEZEL_TEST(type_alias_nullable_pattern_4083, nullable_prim_alias_bare_matches) {
    auto r = runAot(
        "module M; "
        "type AliasNullableInt4083 := int?; "
        "class C "
        "  public static method Main(): int "
        "    var x: int? := 42; "
        "    return (case x of AliasNullableInt4083 => 7, _ => 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.diagnostics.hasErrors(), false);  // must accept the program
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §19.5 (v0.292): a bare NULLABLE-PRIMITIVE-alias pattern on a
// nullable-VALUE scrutinee whose value is ABSENT must NOT match (§19.5
// "a bare type pattern T matches only non-null values" — the alias is
// type-test semantically equivalent to a bare `int?` `TypePattern`,
// which is handled by L11115 with the same `sameUnderlying` predicate).
// Pre-fix the program never reached this point (it crashed); post-fix it
// must reject the alias arm at runtime and the wildcard arm fires —
// value 0.
BEZEL_TEST(type_alias_nullable_pattern_4083, nullable_prim_alias_bare_null_does_not_match) {
    auto r = runAot(
        "module M; "
        "type AliasNullableIntNull4083 := int?; "
        "class C "
        "  public static method Main(): int "
        "    var x: int? := null; "
        "    return (case x of AliasNullableIntNull4083 => 7, _ => 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.diagnostics.hasErrors(), false);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// #4083 issue repro (control): the DIRECT (non-alias) `int?` `TypePattern`
// form over a nullable-VALUE scrutinee continues to work — pins that the
// new alias arm does not disturb the pre-existing `TypePattern`-on-`int?`
// path.
BEZEL_TEST(type_alias_nullable_pattern_4083, direct_primitive_typepattern_unaffected) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var x: int? := 42; "
        "    return (case x of int? => 7, _ => 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.diagnostics.hasErrors(), false);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §19.3 / §27.8 (v0.294): a `type` alias whose underlying kind is a
// FUNCTION TYPE (the `#4062` codegen crash's empty alt-list sibling —
// also a non-Named underlying kind) used as a bare single-member
// NamedPattern on a nullable-VALUE scrutinee rejects LOUDLY (the #1847
// #4083-arm-2 backstop) instead of crashing the IR verifier (matches the
// single-member `FnAlias4048c` shape that already produced a clean
// diagnostic at <file>:6:23 before this commit — pins the parallel fix
// in the isNullableVal branch that #4083 installs).
BEZEL_TEST(type_alias_nullable_pattern_4083,
           fn_alias_on_nullable_int_bare_rejected_not_crashed) {
    auto r = runAot(
        "module M; "
        "type FnAliasNullableInt4083 := (int) -> int; "
        "class C "
        "  public static method Main(): int "
        "    var x: int? := 42; "
        "    return (case x of FnAliasNullableInt4083 => 7, _ => 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kBackstopMsg));
}

// fix #5183 (§19.3 / §19.1 / §27.8, v0.311): an alias is a pure synonym
// (§27.8), so the arm `case x of AliasNonNullableInt4083 => 7` over
// `x: int?` IS the `int` type pattern (§19.3) over an `int?` scrutinee.
// §19.3's refutable null test of a present value applies; the arm
// matches when the value is present (42 → 7). Pre-#5183 the isNullableVal
// branch's bare-NamedPattern alias arm (#4083) gated on
// `underlying->nullable == scrutTy->nullable`, treating a non-nullable
// primitive alias as a non-match and falling through to the
// #1847 / #4083-arm-2 `not supported by the backend` diagnostic — a
// backstop originally installed to prevent an IR-verify crash, not a
// spec verdict. §27.8 + §19.3 + §19.1 do not allow the rejection: the
// alias lowers as the aliased type's pattern (here, `int` over `int?`)
// and the `int` pattern over a nullable value is the refutable null
// test of §19.5, matching exactly when the value is present. The
// `fn_alias_on_nullable_int_bare_rejected_not_crashed` sibling and the
// nullable-alias-on-nullable rows are unaffected.
BEZEL_TEST(type_alias_nullable_pattern_4083,
           non_nullable_prim_alias_on_nullable_scrutinee_runs) {
    auto r = runAot(
        "module M; "
        "type AliasNonNullableInt4083 := int; "
        "class C "
        "  public static method Main(): int "
        "    var x: int? := 42; "
        "    return (case x of AliasNonNullableInt4083 => 7, _ => 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.diagnostics.hasErrors(), false);  // per §27.8 + §19.3, accepted
    BEZEL_EXPECT_EQ(r.exitCode, 7);                     // §19.3 refutable null test matches
}

// fix #5183 (§19.3 / §19.1 / §27.8, v0.311): the sibling — the same
// alias pattern over an UN-NARROWED `int?` parameter. Per §19.1 the
// scrutinee is read after §7.4 narrowing; with no narrowing at the
// `case`, it is `int?` and the arm IS the refutable null test of
// §19.5 (matches only non-null values). `F(null)` does NOT match
// (alias arm rejected, wildcard fires → 0); `F(42)` matches (alias
// arm fires → 7). Pins both the present and absent legs of §19.5
// together with the #5183 flip.
BEZEL_TEST(type_alias_nullable_pattern_4083,
           non_nullable_prim_alias_on_nullable_scrutinee_parameter_unnarrowed) {
    auto rNull = runAot(
        "module M; "
        "type AliasNonNullableInt5183 := int; "
        "class C "
        "  public static method F(x: int?): int "
        "    return (case x of AliasNonNullableInt5183 => 7, _ => 0); "
        "  end F; "
        "  public static method Main(): int "
        "    return C.F(null); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(rNull.diagnostics.hasErrors(), false);
    BEZEL_EXPECT_EQ(rNull.exitCode, 0);  // §19.5: alias arm does NOT match null

    auto rPresent = runAot(
        "module M; "
        "type AliasNonNullableInt5183 := int; "
        "class C "
        "  public static method F(x: int?): int "
        "    return (case x of AliasNonNullableInt5183 => 7, _ => 0); "
        "  end F; "
        "  public static method Main(): int "
        "    return C.F(42); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(rPresent.diagnostics.hasErrors(), false);
    BEZEL_EXPECT_EQ(rPresent.exitCode, 7);  // §19.5: alias arm matches present value
}
