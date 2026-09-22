// fix #4568 (§35.4 The classification / §14.1 Generic types, v0.306):
// `common::substitutedChoicePayloadType` — the FOURTH eager generic-
// substitution walker from #4212's own table, the one `1eadfaf4` left alone —
// was the pre-#4212 `applyGenericSubst` body verbatim in shape: TOP-LEVEL-ONLY.
// A payload declared as a BARE parameter (`owned v: T`) was substituted; a
// payload whose type merely MENTIONED the parameter (`owned inner: Inner<T>`)
// was returned as resolved, with `T` still bare.
//
// ── WHY THAT IS A WRONG ANSWER, NOT A CONSERVATIVE ONE ──────────────────────
//
// §35.4: "For a generic choice the property is evaluated per instantiation
// under §3.6's rule 2." At `Outer<int>` the payload `owned inner: Inner<T>`
// IS `Inner<int>`, whose own `owned v: T` erases to `int` — so `Inner<int>` is
// value-like (rule 3) and `Outer<int>` is value-like with it. What the §28.1
// erasure oracle actually received was the still-generic `Inner<T>`, and a
// still-generic bare parameter classifies conservatively OWNING
// (`ownedMarkerActiveOnInstantiatedTypeImpl`'s null-declaration arm) — so
// `Outer<int>` came out OWNING, i.e. a move-only value type (§15.3). The
// user-visible consequence is an over-rejection of legal code: `var p := o;`
// moves instead of copying, and the next read of `o` draws
//
//   owned value in `o` was already moved out at this point (§28.3)
//
// MEASURED on the pre-fix compiler at this worktree's base: T1, T2, T6 and T7
// below are all rejected; T3, T4 and T5 behave identically on both sides.
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// `substitutedChoicePayloadType` now builds the `name -> concrete` map from
// (`decl->genericParams`, `rt->genericArgs`) and hands the payload's resolved
// TypeRef to the shared `sema::substituteGenericArgs` authority #4212 hoisted,
// which recurses into `genericArgs`, an InlineArray element and Function
// parameter/return slots. `ctx` became non-`const` up the choice-classification
// chain because that walker allocates the substituted TypeRef through
// `SemaContext::makeTypeRef`; every caller already held a non-`const`
// `SemaContext&`, so no call site changed.
//
// ONE arm stays local on purpose and is pinned by T6: the TOP-LEVEL
// bare-parameter lookup reads `TypeRef::headName()`, which is ALIAS-
// TRANSPARENT (#1462 — after `type Id<T> := T;` resolves, `astNode` is
// restored to the use site `Id` and the underlying head `T` lives on
// `aliasResolvedHead`), whereas the shared walker's `paramHead` deliberately
// reads only the use-site single-part head. Delegating that arm too would
// regress `owned v: Id<T>` from value-like to owning.
//
// ── WHAT EACH ROW IS FOR ────────────────────────────────────────────────────
//
//   T1  THE BUG. The payload is NESTED (`Inner<T>`), which is the whole point:
//       a top-level-only walker stops exactly one level too early here. The
//       row copies the local and reads BOTH copies, asserting the payload
//       VALUE through each — not merely that the program compiled.
//   T2  The same instantiation observed through a completely independent
//       consequence of the classification: a move-only value may not be a
//       record field (§35.4 "Owning choices are move-only value types",
//       §15.3), so `record Holder(o: Outer<int>)` is a declaration-site
//       rejection pre-fix. Proves T1 is the classification, not one
//       ownership-site quirk.
//   T3  NEGATIVE CONTROL — the axis that must stay rejected. `Outer<Box>` is
//       genuinely owning (the nested `Inner<Box>` has an `owned` class
//       payload, §35.4 rule 1), so the copy-then-read must STILL draw the
//       §28.3 moved-out diagnostic. Proves the fix relaxed only the value
//       axis and did not turn a real owning choice copyable.
//   T4  CONTROL at the shape the pre-fix walker ALREADY handled: a top-level
//       bare payload (`owned v: T`) at `Flat<int>`. Accepted on both sides —
//       recorded here so it is explicit that a bare-`T` payload cannot detect
//       this defect, which is exactly why the bug survived #4212.
//   T5  CONTROL that isolates SUBSTITUTION from NESTING: the same nested
//       payload written CONCRETELY (`owned inner: Inner<int>` in a
//       non-generic outer). Accepted on both sides — so T1's rejection is
//       attributable to the un-substituted parameter, not to nesting per se.
//   T6  The alias-transparency arm (see THE FIX above): `owned v: Id<T>` with
//       `type Id<T> := T;` must stay value-like at `Flat<int>`. This row
//       fails if the top-level arm is "cleaned up" into the shared walker.
//   T7  Per-INDEX substitution, not just the single-parameter case:
//       `Outer2<T, U>` with payload `owned inner: Inner<U>` at
//       `Outer2<Box, int>` is value-like — the nested mention must resolve
//       through U (a value), not through T (a class).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool hasErrDiagWith(const bezel::test::JitResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// ── T1 — THE BUG: a NESTED generic payload at a value substitution.
// `Outer<int>`'s payload `owned inner: Inner<T>` is `Inner<int>`, whose own
// `owned v: T` erases over `int` (§28.1) — so `Outer<int>` is value-like
// (§35.4 rule 3) and copyable. Pre-fix the second read of `o` was rejected as
// a use after move.
BEZEL_TEST(codegen_generic_choice_nested_payload_substitution_4568,
           nested_generic_payload_value_substitution_is_copyable_spec_35_4) {
    auto r = runAot(
        "module M4568A; "
        "choice Inner<T> A(owned v: T), B(); end Inner; "
        "choice Outer<T> W(owned inner: Inner<T>), X(); end Outer; "
        "class C "
        "  public static method Main(): int "
        "    var o: Outer<int> := Outer<int>.W(Inner<int>.A(41)); "
        "    var p: Outer<int> := o; "
        "    Console.WriteLine(\"o \" + Read(o).ToString()); "
        "    Console.WriteLine(\"p \" + Read(p).ToString()); "
        "    return 0; "
        "  end Main; "
        "  private static method Read(q: Outer<int>): int "
        "    return (case q of "
        "      W(var i) => (case i of A(var v) => v, B => -1), "
        "      X        => -2); "
        "  end Read; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("o 41\np 41\n"));
}

// ── T2 — the same instantiation through a second, independent consequence of
// the classification: §35.4/§15.3 forbid a move-only value as a record field,
// so `record Holder(o: Outer<int>)` was rejected at the declaration pre-fix.
BEZEL_TEST(codegen_generic_choice_nested_payload_substitution_4568,
           nested_generic_payload_value_substitution_is_record_field_spec_15_3) {
    auto r = runAot(
        "module M4568B; "
        "choice Inner<T> A(owned v: T), B(); end Inner; "
        "choice Outer<T> W(owned inner: Inner<T>), X(); end Outer; "
        "record Holder(o: Outer<int>); "
        "class C "
        "  public static method Main(): int "
        "    var h := new Holder(Outer<int>.W(Inner<int>.A(5))); "
        "    Console.WriteLine(\"h \" + (case h.o of "
        "      W(var i) => (case i of A(var v) => v, B => -1), "
        "      X => -2).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("h 5\n"));
}

// ── T3 — NEGATIVE CONTROL. `Outer<Box>`'s nested `Inner<Box>` has an `owned`
// CLASS payload, so it is genuinely owning (§35.4 rule 1) and `Outer<Box>` is
// move-only with it: the copy-then-read must STILL be rejected, and with the
// §28.3 moved-out diagnostic rather than some backstop wording.
BEZEL_TEST(codegen_generic_choice_nested_payload_substitution_4568,
           nested_generic_payload_class_substitution_stays_move_only_spec_15_3) {
    auto r = runAot(
        "module M4568C; "
        "class Box public n: int; "
        "  public constructor(n: int) this.n := n; end constructor; "
        "end Box; "
        "choice Inner<T> A(owned v: T), B(); end Inner; "
        "choice Outer<T> W(owned inner: Inner<T>), X(); end Outer; "
        "class C "
        "  public static method Main(): int "
        "    var o: Outer<Box> := Outer<Box>.W(Inner<Box>.A(new Box(3))); "
        "    var p: Outer<Box> := o; "
        "    Console.WriteLine((case o of W(_) => 1, X => 2).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrDiagWith(
        r, "owned value in `o` was already moved out at this point "
           "(\xC2\xA7""28.3)"));
}

// ── T4 — CONTROL at the shape the pre-fix top-level-only walker already
// handled: a BARE parameter payload. Accepted on both sides; recorded so the
// file states outright that this shape cannot detect the defect.
BEZEL_TEST(codegen_generic_choice_nested_payload_substitution_4568,
           top_level_bare_payload_value_substitution_control_spec_28_1) {
    auto r = runAot(
        "module M4568D; "
        "choice Flat<T> A(owned v: T), B(); end Flat; "
        "class C "
        "  public static method Main(): int "
        "    var o: Flat<int> := Flat<int>.A(7); "
        "    var p: Flat<int> := o; "
        "    Console.WriteLine(\"a \" + (case o of A(var v) => v, "
        "                                          B => -1).ToString()); "
        "    Console.WriteLine(\"b \" + (case p of A(var v) => v, "
        "                                          B => -1).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a 7\nb 7\n"));
}

// ── T5 — CONTROL isolating SUBSTITUTION from NESTING: the identical nested
// payload written concretely in a NON-generic outer. Accepted on both sides,
// so T1's pre-fix rejection is attributable to the unsubstituted parameter.
BEZEL_TEST(codegen_generic_choice_nested_payload_substitution_4568,
           concrete_nested_payload_needs_no_substitution_control_spec_35_4) {
    auto r = runAot(
        "module M4568E; "
        "choice Inner<T> A(owned v: T), B(); end Inner; "
        "choice Outer W(owned inner: Inner<int>), X(); end Outer; "
        "class C "
        "  public static method Main(): int "
        "    var o: Outer := Outer.W(Inner<int>.A(8)); "
        "    var p: Outer := o; "
        "    Console.WriteLine(\"o \" + (case o of "
        "      W(var i) => (case i of A(var v) => v, B => -1), "
        "      X => -2).ToString()); "
        "    Console.WriteLine(\"p \" + (case p of "
        "      W(var i) => (case i of A(var v) => v, B => -1), "
        "      X => -2).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("o 8\np 8\n"));
}

// ── T6 — the deliberately-local ALIAS-TRANSPARENT top-level arm. `type
// Id<T> := T;` leaves the payload's resolved TypeRef pointing at the use-site
// `Id` node with the real head on `aliasResolvedHead` (#1462), which only
// `headName()` sees. If that arm is folded into the shared walker (whose
// `paramHead` reads the use-site head), `Flat<int>` regresses to owning and
// this row fails.
BEZEL_TEST(codegen_generic_choice_nested_payload_substitution_4568,
           alias_headed_bare_payload_still_substitutes_spec_27_8) {
    auto r = runAot(
        "module M4568F; "
        "type Id<T> := T; "
        "choice Flat<T> A(owned v: Id<T>), B(); end Flat; "
        "class C "
        "  public static method Main(): int "
        "    var o: Flat<int> := Flat<int>.A(6); "
        "    var p: Flat<int> := o; "
        "    var n := 0; "
        "    case o of A(_) => n := n + 1, B => n := n + 100 end; "
        "    case p of A(_) => n := n + 10, B => n := n + 200 end; "
        "    Console.WriteLine(\"alias \" + n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("alias 11\n"));
}

// ── T7 — per-INDEX substitution: the nested mention is of the SECOND
// parameter, and the first is a class. `Outer2<Box, int>` must resolve
// `Inner<U>` through U (a value) and classify value-like; reading the index
// from the wrong parameter would classify it owning.
BEZEL_TEST(codegen_generic_choice_nested_payload_substitution_4568,
           nested_payload_resolves_through_the_right_parameter_spec_14_1) {
    auto r = runAot(
        "module M4568G; "
        "class Box public n: int; "
        "  public constructor(n: int) this.n := n; end constructor; "
        "end Box; "
        "choice Inner<T> A(owned v: T), B(); end Inner; "
        "choice Outer2<T, U> W(owned inner: Inner<U>, tag: int), X(); "
        "end Outer2; "
        "class C "
        "  public static method Main(): int "
        "    var o: Outer2<Box, int> := "
        "        Outer2<Box, int>.W(Inner<int>.A(3), 9); "
        "    var p: Outer2<Box, int> := o; "
        "    Console.WriteLine(\"o \" + (case o of W(_, var t) => t, "
        "                                          X => -1).ToString()); "
        "    Console.WriteLine(\"p \" + (case p of W(_, var t) => t, "
        "                                          X => -1).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("o 9\np 9\n"));
}
