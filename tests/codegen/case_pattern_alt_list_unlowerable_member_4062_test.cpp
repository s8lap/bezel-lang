// Spec-derived tests for fix #4062 — §19.3 / §19.5 / §27.8 (v0.294): the
// codegen side of the `ConstantPattern` `|`-alternative-list pattern
// lowering has NO guard for a member that fails to lower to any LLVM
// value — `if (!cv) return nullptr;` simply propagates `nullptr` out
// of `patternMatches`, leaving `case.end` unterminated → LLVM IR
// verifier crash ("Basic Block … does not have terminator" + the
// generic #2491 backstop).
//
// Sema's `#4048` (the alias-of-Named-kind arm in
// `ExprTC::altPartsNameType`) closes one specific source of
// unclassifiable alt-list members (a `type` alias whose aliasedType
// resolves to a `TypeRef::Kind::Named` — class/interface/record/enum),
// but it does NOT close the underlying codegen hole: any alt-list
// member that sema classifies as "not a type" (per the
// `resolveAliasToTypeSymbol` null-is-not-a-type contract — `ExprTC::
// altPartsNameType`'s doc comment in `ExprTypecheck.cpp` calls out
// "Returns null for an alias whose underlying type is not a user
// TypeSymbol (a primitive, a function type, or a bare generic
// parameter)" — those nulls enter `checkAltMembersConstant` as
// constant candidates but have no actual value to lower) reaches
// codegen unhandled.
//
// This fix closes the CODegen guard at both `if (!cv) return nullptr;`
// sites (`LlvmBackend_Exprs2.cpp`, the `ConstantPattern` lowering in
// the nullable-VALUE-scrutinee branch and the non-nullable branch) by
// mirroring the SIBLING single-member `NamedPattern` codegen path at
// L11328–11349 (#1847, #1853): report the §19.3 diagnostic once per
// arm and OR a never-match `false` into `any`, exactly as the
// adjacent #2227 fix does for the type-mismatch case. Closing the
// codegen hole at these two sites makes the path fail LOUDLY for
// EVERY unclassifiable member, regardless of which specific sema gap
// put it there — independent of the parallel symbol-by-symbol
// enumeration family (#4038, #4048, #4050, #4051, #4052).
//
// Test oracle choice: every rejection row below uses
// `failedAtCompileTimeWithRealLocation`
// (tests/support/aot_result_asserts.hpp) — which explicitly EXCLUDES
// a crash/signal/timeout/ASan outcome — AND a diagnostic-text check
// for the specific §19.3 wording, so a row can only pass if the
// compiler produced exactly the intended clean rejection. A bare
// `EXPECT_FALSE(r.ran)` would pass vacuously whether the fix is
// present or not, since a crash also leaves `ran == false`.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
// #4062 codegen-side message: distinct from the #1847 single-member
// backstop the bare-NamedPattern path uses, since this site is the
// `|`-alternative-list codegen lowering. Same §19.3 anchor.
constexpr const char* kAltListMsg =
    "could not be lowered to a value to match against the scrutinee";
}  // namespace

// #4062 issue repro, verbatim (§19.3 / §27.8): a `|`-alternative list
// over an int scrutinee, both members function-type aliases.
// Pre-fix: LLVM IR verifier crash ("Basic Block … does not have
// terminator"); surviving #4048, which closes only the alias-of-Named
// case (the `altPartsNameType` `TypeAliasSymbol` arm). Post-fix: clean
// §19.3 rejection, no crash.
BEZEL_TEST(case_pattern_alt_list_unlowerable_member_4062,
           fn_aliases_in_int_alt_rejected_not_crashed) {
    auto r = runAot(
        "module M; "
        "type FnAliasA4062 := (int) -> int; "
        "type FnAliasB4062 := (int) -> bool; "
        "class C "
        "  public static method Main(): int "
        "    var x: int := 3; "
        "    return (case x of FnAliasA4062 | FnAliasB4062 => 1, _ => 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kAltListMsg));
}

// #4062 sibling shape: the SAME nullable-VALUE-scrutinee `|`-alternative
// path (LlvmBackend_Exprs2.cpp L10900-10903, the `isNullableVal`
// branch's ConstantPattern lowering — also has the `if (!cv) return
// nullptr;` unguarded site). Different scrutinee kind, same codegen
// hole. Without this fix: pre-fix crashes identically. Post-fix:
// clean rejection.
BEZEL_TEST(case_pattern_alt_list_unlowerable_member_4062,
           fn_aliases_in_nullable_int_alt_rejected_not_crashed) {
    auto r = runAot(
        "module M; "
        "type FnAliasNullableA4062 := (int) -> int; "
        "type FnAliasNullableB4062 := (int) -> bool; "
        "class C "
        "  public static method Main(): int "
        "    var x: int? := 3; "
        "    return (case x of FnAliasNullableA4062 | FnAliasNullableB4062 "
        "                        => 1, _ => 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kAltListMsg));
}

// #4062 control from the issue (already-cited): the SINGLE-MEMBER
// (non-alt-list) form of the same `FnAlias4048c`-style alias on a
// matching scrutinee ALREADY produces a clean §19.3 diagnostic at
// <file>:6:23 (the bare-NamedPattern #1847 path is the canonical
// fix point for unclassifiable single-member names). Pre-fix AND
// post-fix this stays a clean rejection — pins that the new alt-list
// guard does not regress the existing single-member backstop.
BEZEL_TEST(case_pattern_alt_list_unlowerable_member_4062,
           fn_alias_single_member_still_rejected_with_diagnostic) {
    auto r = runAot(
        "module M; "
        "type FnAliasSingle4062 := (int) -> int; "
        "class C "
        "  public static method Main(): int "
        "    var x: int := 3; "
        "    return (case x of FnAliasSingle4062 => 1, _ => 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // single-member path uses #1847's wording; alt-list (this fix) uses
    // #4062's wording. Both fire cleanly — distinct messages are fine.
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "not supported by the backend") ||
        diagnosticMessageContains(r, kAltListMsg));
}

// ── ANTI-REGRESSION: the codegen guard only fires on a genuine
// unclassifiable member — a valid `|`-alternative list whose members
// ARE all lowerable constants continues to compile AND match
// correctly, with NO spurious diagnostic.

// §10.6 / §19.3: enum-constant alternatives (no alias involved, both
// members real enumerators) still compile and select correctly.
BEZEL_TEST(case_pattern_alt_list_unlowerable_member_4062,
           enum_constant_alt_still_accepted_and_correct) {
    const std::string prog =
        "module M; "
        "enum Color4062 Red, Green, Blue; end Color4062; "
        "class C "
        "  public static method M(c: Color4062): int "
        "    return (case c of Red | Blue => 1, Green => 2); "
        "  end M; "
        "  public static method Main(): int "
        "    return M(Color4062.Red) * 10 + M(Color4062.Green); "
        "  end Main; "
        "end C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_EQ(r.diagnostics.hasErrors(), false);
    BEZEL_EXPECT_EQ(r.exitCode, 12);  // 1*10 + 2
}

// §17.14 / §19.3: integer-literal alternatives (no alias involved, both
// members real integer literals) still compile and select correctly.
BEZEL_TEST(case_pattern_alt_list_unlowerable_member_4062,
           int_literal_alt_still_accepted_and_correct) {
    const std::string prog =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var x: int := 7; "
        "    return (case x of 1 | 5 | 7 => 42, _ => 0); "
        "  end Main; "
        "end C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_EQ(r.diagnostics.hasErrors(), false);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
