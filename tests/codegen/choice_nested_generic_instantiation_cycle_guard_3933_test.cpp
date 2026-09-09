// fix #3933 (§35.11/§35.4, v0.295): `common::classifyChoiceCore`'s
// `choiceSeen` cycle-guard (src/common/OwnershipFacts.cpp) was keyed on the
// bare `sema::TypeSymbol*` DECLARATION, shared by every instantiation of a
// generic choice (`Pair<int>`, `Pair<Box>`, `Pair<Pair<Box>>` all point at the
// SAME `Pair` TypeSymbol). Classifying `Pair<Pair<X>>` inserts `Pair`'s decl
// into `choiceSeen`, then recurses — still on that stack frame — into
// classifying the INNER `Pair<X>` instantiation: same decl, so the #2159
// cycle guard fired as a false "self-referential cycle" and short-circuited
// to `ValueLike`, even though `Pair<X>` is a completely legitimate,
// non-recursive nesting at a DIFFERENT type argument, not the genuine
// same-instantiation self-containment cycle the guard exists to catch
// (already independently rejected at the declaration by §35.1/§9).
//
// ── A SECOND, ADJACENT BUG FOUND WHILE VERIFYING THIS ONE ───────────────────
//
// Fixing ONLY the `choiceSeen` re-key (OwnershipFacts.cpp) does not, by
// itself, make `Pair<Pair<int>>.Equals` compile — MEASURED on this worktree
// with that one fix alone applied: still rejected, byte-identical diagnostic
// text. The reject survives because `SymbolCollection.cpp`'s
// `isAdmissibleComponentType` — the §9.2 payload-admissibility walk
// `isAutoEqualityBearingChoice` drives — has its OWN, independent bug in its
// Choice arm:
//
//     if (decl->kind == ast::TypeDeclKind::Choice)
//         return isAutoEqualityBearingChoice(decl, ctx);   // no `ty`!
//
// `ty` is the caller's OWN already-substituted, fully-instantiated component
// type (e.g. the inner `Pair<int>` payload of the outer `Pair<Pair<int>>`),
// but it is discarded — so a nested choice component is always judged at its
// BARE, uninstantiated declaration, whose own bare-parameter payload
// conservatively classifies borrow-carrying (§9.7's documented "might carry a
// borrow" posture for an unresolved parameter) — making `Pair` read as
// inadmissible AS A COMPONENT no matter what it is actually instantiated at.
// That is the load-bearing cause of the `Pair<Pair<int>>` false reject; the
// `choiceSeen` bug above only produces a WRONG DIAGNOSTIC REASON for the
// separately-and-correctly-rejected `Pair<Pair<Box>>` (see T4/T5 below), not
// a wrong verdict. Both are fixed here (OwnershipFacts.cpp + the
// `isAutoEqualityBearingChoice(decl, ctx, ty)` one-argument fix in
// SymbolCollection.cpp) since the filed issue's own repros do not compile
// clean without both.
//
// ── WHAT EACH ROW IS FOR ─────────────────────────────────────────────────────
//
//   T1  the filed accept-side repro (`Pair<Pair<int>>`, every payload `int`
//       "all the way down"): `.Equals` now compiles and answers correctly for
//       both an equal pair (True) and a differing pair (False) — not just
//       "sema now accepts it".
//   T2  the SAME shape through the `=` operator (ExprTypecheck_Exprs.cpp — a
//       completely different call site from `.Equals`'s
//       checkEqualityMemberUseSite, proving the fix is not specific to one
//       consumer of the shared classifyChoiceCore/isAutoEqualityBearingChoice
//       predicates).
//   T3  `.GetHashCode()` at the same doubly-nested value substitution: two
//       equal `Pair<Pair<int>>` values hash equal (§8.3) — the hash path, not
//       only the equality path.
//   T4  the filed reject-side repro (`Pair<Pair<Box>>`, MEASURED "safe-
//       direction" pre-fix: wrong reason, correct reject) via `.Equals`: MUST
//       still reject (Box is a class — genuinely borrow-carrying, §35.11),
//       and now with the CORRECT reason text ("the choice is borrow-bearing")
//       instead of the pre-fix "case payload is not an admissible equality
//       component" text a reader would misdiagnose as an admissibility
//       problem rather than a classification one.
//   T5  the SAME `Pair<Pair<Box>>` shape through the `=` operator — the
//       second call site, same correct-reason assertion.
//   T6  a THIRD level of nesting (`Pair<Pair<Pair<int>>>`) — the guard's old
//       bare-decl key collides identically at any nesting depth beyond one,
//       so this pins the fix generalizes past the filed issue's two-level
//       repro rather than special-casing depth 2.
#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

// ── T1 — `Pair<Pair<int>>.Equals`: accept, correct runtime answer for both an
// equal and a differing doubly-nested payload.
BEZEL_TEST(codegen_choice_nested_generic_instantiation_cycle_guard_3933,
           pair_pair_int_equals_correct_answer_spec_35_11) {
    auto r = runAot(
        "module M3933A; "
        "choice Pair<T> First(value: T), Second(); end Pair; "
        "class C "
        "  public static method Main(): int "
        "    var a: Pair<Pair<int>> := Pair<Pair<int>>.First(Pair<int>.First(3)); "
        "    var b: Pair<Pair<int>> := Pair<Pair<int>>.First(Pair<int>.First(3)); "
        "    var c: Pair<Pair<int>> := Pair<Pair<int>>.First(Pair<int>.First(4)); "
        "    Console.WriteLine(\"eq \" + a.Equals(b).ToString()); "
        "    Console.WriteLine(\"ne \" + a.Equals(c).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("eq True\nne False\n"));
}

// ── T2 — the SAME doubly-nested shape through the `=` operator (a different
// call site — ExprTypecheck_Exprs.cpp's `typeBinary`, not
// checkEqualityMemberUseSite).
BEZEL_TEST(codegen_choice_nested_generic_instantiation_cycle_guard_3933,
           pair_pair_int_operator_eq_correct_answer_spec_35_11) {
    auto r = runAot(
        "module M3933B; "
        "choice Pair<T> First(value: T), Second(); end Pair; "
        "class C "
        "  public static method Main(): int "
        "    var a: Pair<Pair<int>> := Pair<Pair<int>>.First(Pair<int>.First(3)); "
        "    var b: Pair<Pair<int>> := Pair<Pair<int>>.First(Pair<int>.First(3)); "
        "    var c: Pair<Pair<int>> := Pair<Pair<int>>.First(Pair<int>.First(4)); "
        "    Console.WriteLine(\"eq \" + (a = b).ToString()); "
        "    Console.WriteLine(\"ne \" + (a = c).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("eq True\nne False\n"));
}

// ── T3 — `Pair<Pair<int>>.GetHashCode()`: two equal doubly-nested values hash
// equal (§8.3) — the hash path, not merely un-rejected equality.
BEZEL_TEST(codegen_choice_nested_generic_instantiation_cycle_guard_3933,
           pair_pair_int_gethashcode_agrees_spec_8_3) {
    auto r = runAot(
        "module M3933C; "
        "choice Pair<T> First(value: T), Second(); end Pair; "
        "class C "
        "  public static method Main(): int "
        "    var a: Pair<Pair<int>> := Pair<Pair<int>>.First(Pair<int>.First(3)); "
        "    var b: Pair<Pair<int>> := Pair<Pair<int>>.First(Pair<int>.First(3)); "
        "    Console.WriteLine(\"hash \" "
        "      + (a.GetHashCode() = b.GetHashCode()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("hash True\n"));
}

// ── T4 — NEGATIVE CONTROL, `.` form: `Pair<Pair<Box>>.Equals` — a class-typed
// leaf (`Box`) makes `Pair<Box>` genuinely borrow-carrying, so the OUTER
// `Pair<Pair<Box>>` is genuinely borrow-bearing too (§35.11 transitivity) and
// MUST still reject — but now for the CORRECT reason. MEASURED pre-fix: same
// reject, but reason text "the choice is value-like but a case payload is not
// an admissible equality component (§9.2)" (WRONG — `classifyChoiceCore`'s
// guard bug misclassified the outer choice as value-like). Asserting the
// POST-FIX text means this row also fails-without-the-fix (on message text),
// even though `r.ran` alone does not move.
BEZEL_TEST(codegen_choice_nested_generic_instantiation_cycle_guard_3933,
           pair_pair_box_equals_still_rejects_correct_reason_spec_35_11) {
    auto r = runAot(
        "module M3933D; "
        "choice Pair<T> First(value: T), Second(); end Pair; "
        "class Box public n: int; "
        "  public constructor(n: int) this.n := n; end constructor; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(3); "
        "    var inner: Pair<Box> := Pair<Box>.First(box); "
        "    var a: Pair<Pair<Box>> := Pair<Pair<Box>>.First(inner); "
        "    var b: Pair<Pair<Box>> := Pair<Pair<Box>>.First(inner); "
        "    var eq := a.Equals(b); "
        "    Console.WriteLine(eq.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "it is not auto-hashable (\xC2\xA7""35.7) \xE2\x80\x94 the choice "
           "is borrow-bearing"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "case payload is not an admissible equality component"));
}

// ── T5 — the SAME negative control through the `=` operator.
BEZEL_TEST(codegen_choice_nested_generic_instantiation_cycle_guard_3933,
           pair_pair_box_operator_eq_still_rejects_correct_reason_spec_35_11) {
    auto r = runAot(
        "module M3933E; "
        "choice Pair<T> First(value: T), Second(); end Pair; "
        "class Box public n: int; "
        "  public constructor(n: int) this.n := n; end constructor; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(3); "
        "    var inner: Pair<Box> := Pair<Box>.First(box); "
        "    var a: Pair<Pair<Box>> := Pair<Pair<Box>>.First(inner); "
        "    var b: Pair<Pair<Box>> := Pair<Pair<Box>>.First(inner); "
        "    var eq := (a = b); "
        "    Console.WriteLine(eq.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "it is not auto-hashable (\xC2\xA7""35.7) \xE2\x80\x94 the choice "
           "is borrow-bearing"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "case payload is not an admissible equality component"));
}

// ── T6 — THREE levels of nesting (`Pair<Pair<Pair<int>>>`): the old bare-decl
// key collides identically at any depth beyond one, so this pins the fix
// generalizes past the filed issue's two-level repro.
BEZEL_TEST(codegen_choice_nested_generic_instantiation_cycle_guard_3933,
           pair_pair_pair_int_equals_three_levels_spec_35_11) {
    auto r = runAot(
        "module M3933F; "
        "choice Pair<T> First(value: T), Second(); end Pair; "
        "class C "
        "  public static method Main(): int "
        "    var a: Pair<Pair<Pair<int>>> := Pair<Pair<Pair<int>>>.First( "
        "      Pair<Pair<int>>.First(Pair<int>.First(3))); "
        "    var b: Pair<Pair<Pair<int>>> := Pair<Pair<Pair<int>>>.First( "
        "      Pair<Pair<int>>.First(Pair<int>.First(3))); "
        "    var c: Pair<Pair<Pair<int>>> := Pair<Pair<Pair<int>>>.First( "
        "      Pair<Pair<int>>.First(Pair<int>.First(4))); "
        "    Console.WriteLine(\"eq \" + a.Equals(b).ToString()); "
        "    Console.WriteLine(\"ne \" + a.Equals(c).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("eq True\nne False\n"));
}
