// fix #4316 / spec §5.1 *Implicit conversions* / §11.2 *Interface
// declaration* / §15.4 *Collection literals* (v0.298).
//
// `fix #4310` (tests/sema/collection_literal_element_interface_conformance_
// 4310_test.cpp) closed the §5.1/§11.2 interface-conformance gap for a
// collection literal typed against a known target at six positions
// (LocalDecl, LocalConstDecl, FieldDecl, PropertyDecl auto-initializer,
// ReturnStmt, and the general call/constructor-argument path). #4310's own
// filing named #4316 as a SEVENTH, STRICTLY LARGER gap: a plain
// `AssignmentStmt` reassignment (`outer := [...]`, not a declaration) runs
// NEITHER the §15.4 admissibility gate NOR the per-element conformance
// check — the other six positions at least ran `isCollectionLiteralAdmissibleTarget`
// before #4310 landed.
//
// ROOT CAUSE (traced, ExprTypecheck.cpp's `AssignmentStmt` arm, the
// `as->op == ":="` block). Nothing sets a collection literal's own
// `exprType` at ANY #4310-family position — not even the ones #4310 already
// fixed (see that function's own header comment) — so `TypeRef* fromTy =
// ctx_.exprType(*as->value);` is simply null for a `CollectionLiteralExpr`
// RHS, and the `checkAssignable(fromTy, toTy, ...)` call that follows
// silently no-ops on its `if (!from) return` guard. Every other position
// papers over this with its OWN admissibility gate
// (`isCollectionLiteralAdmissibleTarget`) run *before* `checkAssignable`;
// `AssignmentStmt` ran neither that gate nor the per-element walk — the RHS
// reached codegen completely unchecked. MEASURED SIGSEGV (#4302's exact
// signature): a non-conforming class stored through `outer[0].GetV()`
// dispatches a null itable slot.
//
// THE FIX mirrors the ReturnStmt/LocalDecl/FieldDecl positions exactly:
// gate on §15.4 membership first (the SAME three diagnostics those
// positions already report), then call #4310's existing
// `ExprTC::checkCollectionLiteralElementAssignability` — an EIGHTH caller
// of that one shared authority, not a hand-rolled copy (LANE_RULES §6/§25).
//
// A user-defined INDEXER SETTER (`h[i] := [...]`) goes through the exact
// same `AssignmentStmt` handler — `toTy = ctx_.exprType(*as->target)` is
// generic over any lvalue shape — so the fix covers it too with no separate
// code path (confirmed below: `indexer_setter_noconform_rejected`).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Same diagnostic text #4310's own suite asserts — this fix reuses
// `checkCollectionLiteralElementAssignability`, the identical call, so
// every interface-conformance reject row below asserts identical text.
bool hasNotImplicitlyConvertible(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    return false;
}

// The §15.4 admissibility-gate diagnostic (`reportCollectionLiteralBadTarget`
// / `reportCollectionLiteralDictFormBadTarget`) — distinct from the
// interface-conformance one above: this fires when the TARGET TYPE itself
// is not on §15.4's closed list (or the literal's form does not match the
// target), independent of any element's own type.
bool hasCollectionLiteralBadTarget(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("cannot target the type") != std::string::npos &&
            d.message.find("15.4") != std::string::npos)
            return true;
    return false;
}

const std::string kPrelude =
    "module M; import System.Collections.Generic; "
    "interface IHolder method GetV(): string; end IHolder; "
    "class Widget3 "
    "  public V: string; "
    "  public constructor(v: string) V := v; end constructor; "
    "  public method GetV(): string return V; end GetV; "
    "end Widget3; "
    "class Widget3Ok: IHolder "
    "  public V: string; "
    "  public constructor(v: string) V := v; end constructor; "
    "  public method GetV(): string return V; end GetV; "
    "end Widget3Ok; "
    "class Widget4Ok: IHolder "
    "  public V: string; "
    "  public constructor(v: string) V := v; end constructor; "
    "  public method GetV(): string return V; end GetV; "
    "end Widget4Ok; ";

JitResult run(const std::string& mainBody,
              const std::string& extraMember = "") {
    return runAot(kPrelude +
                  "class C public static method Main(): int " +
                  mainBody + " end Main; " + extraMember + " end C;");
}

}  // namespace

// ============================================================
// REJECT ROWS
// ============================================================

// #4302's exact signature at this position: an inline-array-typed local,
// plain reassignment, non-conforming element.
BEZEL_TEST(assignment_stmt_collection_literal_element_interface_conformance_4316,
           inline_array_noconform_rejected_spec_5_1_11_2) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var outer: [1]IHolder; "
        "outer := [new Widget3(s)]; "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The heap-`Array<T>` twin — reproduces identically per the issue's own
// measurement.
BEZEL_TEST(assignment_stmt_collection_literal_element_interface_conformance_4316,
           heap_array_noconform_rejected_spec_5_1_11_2) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var w := new Widget3(s); "
        "var arr: Array<IHolder>; "
        "arr := [w]; "
        "return arr[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The indexer-setter form (`h[i] := [...]`) — confirmed on #4316 as the
// IDENTICAL `AssignmentStmt` code path (`toTy = ctx_.exprType(*as->target)`
// is generic over any lvalue shape, plain identifier or indexer-set alike).
// No accept-side control for the CONFORMING twin of this exact shape: MEASURED
// on the post-fix compiler, a genuinely conforming element at this same
// indexer-setter position still degrades via the pre-existing "internal:
// indexer-setter RHS expression failed to lower (degraded — target retains
// its previous value) — #3025" backstop and never actually WRITES through
// (`IndexOutOfRangeException` at the read) — a codegen materialization gap
// this issue's own filing already named as pre-existing and unrelated to the
// sema-level admissibility/conformance question this fix answers. Filed
// separately with a measured repro rather than silently working around it.
BEZEL_TEST(assignment_stmt_collection_literal_element_interface_conformance_4316,
           indexer_setter_noconform_rejected_spec_5_1_11_2) {
    auto r = runAot(
        kPrelude +
        "class Holder2 "
        "  private _items: Array<IHolder>; "
        "  public constructor() _items := []; end constructor; "
        "  public property [i: int]: Array<IHolder> "
        "    get => _items; "
        "    set => _items := value; "
        "  end; "
        "end Holder2; "
        "class C public static method Main(): int "
        "  var s := \"he\" + \"llo\"; "
        "  var h := new Holder2(); "
        "  h[0] := [new Widget3(s)]; "
        "  return h[0][0].GetV().Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The §15.4 ADMISSIBILITY half of the gap, independent of interface
// conformance: `AssignmentStmt` ran no admissibility gate at all, so a
// literal reassigned to a target NOT on §15.4's closed list (`Object`) was
// unchecked and reached codegen's inline-array arm — before this fix, a
// non-diagnosed ICE path; after it, a clean §15.4 rejection.
BEZEL_TEST(assignment_stmt_collection_literal_element_interface_conformance_4316,
           non_admissible_target_rejected_spec_15_4) {
    auto r = run(
        "var o: Object; "
        "o := [1, 2, 3]; "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasCollectionLiteralBadTarget(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The FORM half of the same admissibility gate: a dictionary-form literal
// reassigned to a sequence-only target (`Array<int>` admits the sequence
// form only).
BEZEL_TEST(assignment_stmt_collection_literal_element_interface_conformance_4316,
           dict_form_at_sequence_target_rejected_spec_15_4) {
    auto r = run(
        "var arr: Array<int>; "
        "arr := [1: 2]; "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================
// ACCEPT-SIDE CONTROLS — must keep compiling and produce the correct value.
// A false reject here would be worse than the bug this fix closes.
// ============================================================

// NOTE: switched to `[1 owned]` / `[2 owned]` (the spec-conformant form per
// §15.1.1 / #4336 / `569a680`): the non-owning `[N]IHolder` is a BORROW
// slot per §15.1 and the `new Widget3Ok(s)` initializer is now rejected by
// #4351's sema fix. The owning form takes ownership of the fresh widget on
// construction and exercises the SAME AssignmentStmt collection-literal
// element conformance path this test pins.
BEZEL_TEST(assignment_stmt_collection_literal_element_interface_conformance_4316,
           conforming_single_element_accepted_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var outer: [1 owned]IHolder; "
        "outer := [new Widget3Ok(s)]; "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// See note above: switched to `[2 owned]` for #4351.
BEZEL_TEST(assignment_stmt_collection_literal_element_interface_conformance_4316,
           two_different_conforming_implementors_accepted_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var outer: [2 owned]IHolder; "
        "outer := [new Widget3Ok(s), new Widget4Ok(\"world\")]; "
        "return outer[0].GetV().Length + outer[1].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// A nullable interface element, reassigned to a literal holding `null` —
// must stay accepted (the element check only judges a NON-null element
// expression's own type).
BEZEL_TEST(assignment_stmt_collection_literal_element_interface_conformance_4316,
           nullable_interface_element_null_accepted_control) {
    auto r = run(
        "var outer: Array<IHolder?>; "
        "outer := [null]; "
        "return (if outer[0] = null then 42 else 0);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// The empty-literal §15.4 carve-out ("admitted at every collection target
// listed") must still apply at this position.
BEZEL_TEST(assignment_stmt_collection_literal_element_interface_conformance_4316,
           empty_literal_reassignment_accepted_control) {
    auto r = run(
        "var outer: Array<IHolder>; "
        "outer := []; "
        "return outer.Count;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// An ordinary, non-interface reassignment — proves the general
// `AssignmentStmt` path stays fully usable (admissibility gate + element
// check are both no-ops for a plain `int` element type).
BEZEL_TEST(assignment_stmt_collection_literal_element_interface_conformance_4316,
           plain_admissible_non_interface_reassignment_accepted_control) {
    auto r = run(
        "var xs: Array<int>; "
        "xs := [1, 2, 3]; "
        "return xs[0] + xs[1] + xs[2];");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// No `indexer_setter_conforming_accepted_control` row: MEASURED, the
// conforming twin of the reject row above does not presently produce a
// usable value — codegen's indexer-setter RHS materializer has no case for
// a collection literal at all (the pre-existing "internal: indexer-setter
// RHS expression failed to lower ... — #3025" backstop fires, and the
// write silently does not happen), independent of whether the element
// conforms. That is a codegen gap, not a sema false-reject; see the
// header comment on `indexer_setter_noconform_rejected_spec_5_1_11_2`
// above.
