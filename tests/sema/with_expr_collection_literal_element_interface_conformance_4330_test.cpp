// fix #4330 / spec §9.5 *Non-destructive update: `with`* / §5.1 *Implicit
// conversions* / §11.2 *Interface declaration* / §15.4 *Collection
// literals* (v0.298).
//
// `fix #4310` (tests/sema/collection_literal_element_interface_conformance_
// 4310_test.cpp) closed the §5.1/§11.2 interface-conformance gap for a
// collection literal typed against a known target at six positions.
// #4330 is a further position with the SAME severity, found auditing the
// remaining unexplored §15.4 target-typing sites: a record's `with(...)`
// non-destructive-update clause (§9.5).
//
// ROOT CAUSE (traced, ExprTypecheck_Exprs.cpp's `WithExpr` handler). Unlike
// every #4310-family position, the `with`-clause value is NEVER target-typed
// at all: the handler visits each `InitAssign.value` (`if (ia.value)
// visitExpr(*ia.value);`) BEFORE it even looks up the target field's type —
// there is no `ctx_.setExprType`, no §15.4 admissibility gate
// (`isCollectionLiteralAdmissibleTarget`), and (consequently) #4310's
// `checkCollectionLiteralElementAssignability` was never reached either.
// MEASURED SIGSEGV (#4302's signature): `b1 with(Items :=
// [new Widget3Bad(s)])` for a `Widget3Bad` that never lists `IHolder`
// compiles with zero diagnostics; `b2.Items[0].GetV()` dispatches through a
// null itable slot.
//
// THE FIX runs the SAME admissibility-gate-then-per-element-walk pattern
// #4310's other positions use, at BOTH `with`-target arms this handler
// has — the auto-/positional-property arm (`PropertySymbol`) and the
// explicit-field arm (`FieldSymbol`) — calling #4310's existing
// `ExprTC::checkCollectionLiteralElementAssignability` rather than a
// hand-rolled copy (LANE_RULES §6/§25).
//
// A SECOND, COMPOUNDING GAP at the same site — the §9.7/§28.3 borrow-
// lifetime "no-temporary-source" rule that already rejects
// `new Box([new Widget3Bad(s)])` at ordinary construction — is NOT fixed
// here. That check (`MethodOwnership::checkRecordFromTemporary`) lives
// entirely in `src/ownership/Ownership.cpp` / `Ownership_Checks.cpp`,
// both held by another lane this cycle; filed as a new issue (see the
// commit body) rather than fixed silently or left unrecorded.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Same diagnostic text #4310's own suite asserts — this fix reuses
// `checkCollectionLiteralElementAssignability`, the identical call.
bool hasNotImplicitlyConvertible(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    return false;
}

// The §15.4 admissibility-gate diagnostic — distinct from the interface-
// conformance one above.
bool hasCollectionLiteralBadTarget(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("cannot target the type") != std::string::npos &&
            d.message.find("15.4") != std::string::npos)
            return true;
    return false;
}

const std::string kPrelude =
    "module M; "
    "interface IHolder method GetV(): string; end IHolder; "
    "class Widget3Bad "
    "  public V: string; "
    "  public constructor(v: string) V := v; end constructor; "
    "  public method GetV(): string return V; end GetV; "
    "end Widget3Bad; "
    "class Widget3Ok: IHolder "
    "  public V: string; "
    "  public constructor(v: string) V := v; end constructor; "
    "  public method GetV(): string return V; end GetV; "
    "end Widget3Ok; "
    "class Widget4Ok: IHolder "
    "  public V: string; "
    "  public constructor(v: string) V := v; end constructor; "
    "  public method GetV(): string return V; end GetV; "
    "end Widget4Ok; "
    // Positional-field record (§9.1) — `Items` is an auto-generated
    // positional-field PROPERTY, exercising the `PropertySymbol` `with`
    // arm.
    "record Box(Items: [1]IHolder); "
    // Body record (§9.2) with an EXPLICIT field declaration (not an
    // auto-property) — exercises the `FieldSymbol` `with` arm.
    "record BodyBox "
    "  public Items: [1]IHolder; "
    "  public Name: string; "
    "  public constructor(items: [1]IHolder, name: string) "
    "    Items := items; Name := name; "
    "  end constructor; "
    "end BodyBox; ";

JitResult run(const std::string& mainBody) {
    return runAot(kPrelude +
                  "class C public static method Main(): int " +
                  mainBody + " end Main; end C;");
}

}  // namespace

// ============================================================
// REJECT ROWS
// ============================================================

// #4330's own repro, verbatim: the positional-field / PropertySymbol arm.
BEZEL_TEST(with_expr_collection_literal_element_interface_conformance_4330,
           positional_field_inline_array_noconform_rejected_spec_9_5_5_1_11_2) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var items1: [1]IHolder := [new Widget3Ok(s)]; "
        "var b1 := new Box(items1); "
        "var b2 := b1 with(Items := [new Widget3Bad(s)]); "
        "return b2.Items[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The explicit-field / FieldSymbol arm — a DIFFERENT branch of the same
// `WithExpr` handler (`dynamic_cast<FieldSymbol*>(mem)` vs
// `dynamic_cast<PropertySymbol*>(mem)`), fixed separately since neither arm
// shares code with the other beyond the newly-added shared gate.
BEZEL_TEST(with_expr_collection_literal_element_interface_conformance_4330,
           body_record_explicit_field_noconform_rejected_spec_9_5_5_1_11_2) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var items1: [1]IHolder := [new Widget3Ok(s)]; "
        "var b1 := new BodyBox(items1, \"orig\"); "
        "var b2 := b1 with(Items := [new Widget3Bad(s)]); "
        "return b2.Items[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The §15.4 ADMISSIBILITY half of the gap, independent of interface
// conformance: a `with`-clause value targeting a field type NOT on §15.4's
// closed list (`Object`) was previously unchecked at this position.
BEZEL_TEST(with_expr_collection_literal_element_interface_conformance_4330,
           non_admissible_target_rejected_spec_15_4) {
    auto r = runAot(
        "module M; "
        "record Holder(Slot: Object); "
        "class C public static method Main(): int "
        "  var h1 := new Holder(42); "
        "  var h2 := h1 with(Slot := [1, 2, 3]); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasCollectionLiteralBadTarget(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================
// ACCEPT-SIDE CONTROLS — must keep compiling and produce the correct value.
// A false reject here would be worse than the bug this fix closes.
// ============================================================

BEZEL_TEST(with_expr_collection_literal_element_interface_conformance_4330,
           positional_field_conforming_accepted_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "using var w1 := new Widget3Ok(s); "
        "using var w2 := new Widget3Ok(s); "
        "var items1: [1]IHolder := [w1]; "
        "var b1 := new Box(items1); "
        "var b2 := b1 with(Items := [w2]); "
        "return b2.Items[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

BEZEL_TEST(with_expr_collection_literal_element_interface_conformance_4330,
           body_record_explicit_field_conforming_accepted_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "using var w1 := new Widget3Ok(s); "
        "using var w2 := new Widget3Ok(s); "
        "var items1: [1]IHolder := [w1]; "
        "var b1 := new BodyBox(items1, \"orig\"); "
        "var b2 := b1 with(Items := [w2]); "
        "return b2.Items[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// Two DIFFERENT conforming implementors in one `with`-clause collection
// literal, both accepted — the control the element-conformance walk must not
// over-reject.
//
// The carrier is `[2]IHolder` with `using var` element bindings, the same
// shape every other ACCEPT row in this file uses. It was written
// `[2 owned]IHolder` with inline `new` elements, which §9.7 (v0.307) forbids
// outright: "Records also cannot have inline-array fields whose slots carry
// the `owned` marker (`[N owned]T?` or `[N owned]T`); such fields are
// move-only value types (§15.3), and records support only copy semantics."
// The non-owning carrier then requires borrow elements rather than fresh
// producers, per §15.1 — hence the `using var` locals, exactly as
// `positional_field_conforming_accepted_control` above. Only the carrier
// spelling moved; the subject (two distinct `IHolder` implementors in a
// `with`-clause literal, accepted, 5 + 5 == 10) is unchanged.
BEZEL_TEST(with_expr_collection_literal_element_interface_conformance_4330,
           two_different_conforming_implementors_accepted_control) {
    auto r = runAot(
        "module M; "
        "interface IHolder method GetV(): string; end IHolder; "
        "class Widget3Ok: IHolder "
        "  public V: string; "
        "  public constructor(v: string) V := v; end constructor; "
        "  public method GetV(): string return V; end GetV; "
        "end Widget3Ok; "
        "class Widget4Ok: IHolder "
        "  public V: string; "
        "  public constructor(v: string) V := v; end constructor; "
        "  public method GetV(): string return V; end GetV; "
        "end Widget4Ok; "
        "record Box2(Items: [2]IHolder); "
        "class C public static method Main(): int "
        "  var s := \"he\" + \"llo\"; "
        "  using var w1 := new Widget3Ok(s); "
        "  using var w2 := new Widget4Ok(s); "
        "  using var w3 := new Widget3Ok(s); "
        "  using var w4 := new Widget4Ok(\"world\"); "
        "  var items1: [2]IHolder := [w1, w2]; "
        "  var b1 := new Box2(items1); "
        "  var b2 := b1 with(Items := [w3, w4]); "
        "  return b2.Items[0].GetV().Length + b2.Items[1].GetV().Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// A nullable interface element, replaced with a literal holding `null` —
// must stay accepted (the element check only judges a NON-null element
// expression's own type). Uses an INLINE array (`[1]IHolder?`), not a heap
// `Array<IHolder?>`: MEASURED, a heap-`Array<T>`-typed `with`-clause
// replacement value hits an UNRELATED, PRE-EXISTING codegen gap — "internal:
// lowerExpr returned null for with-expression field/property replacement
// value — #1386" — because the `with`-expression lowering path has no
// materializer for a HEAP collection literal at all (every OTHER accept
// control in this file that passes uses an inline-array target for the same
// reason). Filed separately (see the commit body) rather than routed around
// silently.
BEZEL_TEST(with_expr_collection_literal_element_interface_conformance_4330,
           nullable_element_null_accepted_control) {
    auto r = runAot(
        "module M; "
        "interface IHolder method GetV(): string; end IHolder; "
        "record NullableBox(Items: [1]IHolder?); "
        "class C public static method Main(): int "
        "  var items1: [1]IHolder? := [null]; "
        "  var b1 := new NullableBox(items1); "
        "  var b2 := b1 with(Items := [null]); "
        "  return (if b2.Items[0] = null then 42 else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// No `empty_literal_accepted_control` row: the §15.4 empty-literal carve-out
// is inline-array-INADMISSIBLE by construction (inline arrays require >= 1
// element regardless of form, §15.4) — a meaningful empty-literal control at
// this position needs a HEAP collection target, which hits the SAME
// pre-existing #1386 codegen gap the comment above documents (MEASURED:
// `b1 with(Items := [])` against a heap `Array<IHolder>`-typed field also
// produces "internal: lowerExpr returned null for with-expression
// field/property replacement value"). This fix's own admissibility gate
// does admit the empty literal at a heap target correctly (no FALSE reject —
// confirmed by the absence of this fix's own diagnostics in the internal-
// error program above); codegen simply cannot yet materialize the result.

// An UNRELATED field's `with` value (a plain `string`, not a collection
// literal) on the SAME body record used above — proves the new gate
// (`gateWithTargetCollectionLiteral`, which returns `false`/no-op for any
// non-`CollectionLiteralExpr` value) leaves the pre-existing §5.1 mismatch
// check (fix #3177) fully intact.
BEZEL_TEST(with_expr_collection_literal_element_interface_conformance_4330,
           unrelated_string_field_type_mismatch_still_rejected_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var items1: [1]IHolder := [new Widget3Ok(s)]; "
        "var b1 := new BodyBox(items1, \"orig\"); "
        "var b2 := b1 with(Name := 5); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}
