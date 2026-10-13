// Spec-derived tests for issue #1682 — spec §22.3 "`ref` and `out` argument
// locations" (v0.184): a `ref`/`out` argument must be an assignable storage
// location (a mutable local variable, a writable field, an indexer slot, or
// a property with an accessible setter), subject to: no `using var` binding
// (§28.10's pin); no readonly field outside its assignable context (§22.8);
// no location of OWNED state — an owned local or `owned` parameter, an
// `owned` field, an owning-collection or owning-inline-array slot; and no
// mutable static admitted by §22.12 rule 2.
//
// Before this fix the compiler enforced NONE of these categories:
//   - an invalid non-lvalue operand (a method-call result, an arbitrary
//     expression) reached codegen's lvalueAddr, which returned null, and
//     buildOrderedArgs reported "internal: lowerExpr returned null ... —
//     #1386" — an internal-error-style message, not a clean §22.3 diagnostic
//     naming the actual rule;
//   - a readonly field, a plain owned local, an owned field, and an owning-
//     collection/owning-inline-array indexer slot were all silently
//     ACCEPTED as ref/out operands (compiled clean, rc=0) — the owned-state
//     cases are a genuine soundness gap: a `ref` write is an assignment
//     through an alias, invisible to the drop-on-overwrite (§28.7) /
//     consume-site-freeze (§28.3) machinery owned state relies on.
//
// Each NEGATIVE test below asserts the SPECIFIC new §22.3-cited diagnostic
// is present (not just "some error exists" — the old ICE/internal message
// also satisfies hasErrors(), so a bare hasErrors() check would not
// distinguish the fix from the pre-fix state). Each POSITIVE control asserts
// no §22.3 diagnostic fires for a category the section admits.

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// Works for both a JitResult (the `runAot` path) and an OwnershipResult (the
// `parseAndCheckOwnership` path) — both expose a `.diagnostics`
// DiagnosticEngine. The owned-state ref/out rejections (§22.3's dataflow half)
// are emitted by the OWNERSHIP pass; those tests therefore drive
// `parseAndCheckOwnership` directly, so the assertion pins that pass itself.
// (The deprecated in-process JIT harness never carried that pass's diagnostics
// into its result, which is why these rows were not written against it.)
template <class Result>
bool ownershipDiagContains(const Result& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// =============================================================================
// NEGATIVE — invalid operand categories must be rejected with a §22.3
// diagnostic (fails without the fix: either compiles clean, or the ONLY
// error is the unrelated #1386 internal-error message).
// =============================================================================

// §22.3: a `ref` argument must be an assignable storage location — a bare
// method-call result is not one (grammar A.6's lvalue shape). Pre-fix this
// reached codegen and produced "internal: lowerExpr returned null ... —
// #1386", not a §22.3 diagnostic.
BEZEL_TEST(ref_out_operand_category_1682, ref_to_call_result_rejected_22_3) {
    auto r = runAot(
        "module M; class C "
        "  public static method GetInt(): int return 5; end GetInt; "
        "  public static method Bump(ref x: int) x := x + 1; end Bump; "
        "  public static method Main(): int "
        "    Bump(ref GetInt()); return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.3"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "internal:"));
}

// §22.3: an arbitrary expression (not even a call) is likewise not a
// storage location.
BEZEL_TEST(ref_out_operand_category_1682, ref_to_arbitrary_expression_rejected_22_3) {
    auto r = runAot(
        "module M; class C "
        "  public static method Bump(ref x: int) x := x + 1; end Bump; "
        "  public static method Main(): int "
        "    var a := 1; var b := 2; "
        "    Bump(ref (a + b)); return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.3"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "internal:"));
}

// §22.3 / §22.8: a readonly field is assignable only in a field initializer
// or the declaring class's constructor; passing it by `ref` from an
// ordinary method is outside that context. Pre-fix: compiled clean (rc=0),
// no diagnostic at all.
BEZEL_TEST(ref_out_operand_category_1682, ref_to_readonly_field_outside_ctor_rejected_22_3) {
    auto r = runAot(
        "module M; class C "
        "  public readonly V: int; "
        "  public constructor(v: int) this.V := v; end constructor; "
        "  public static method Bump(ref x: int) x := x + 1; end Bump; "
        "  public static method Main(): int "
        "    using var c := new C(5); "
        "    Bump(ref c.V); return c.V; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.3"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.8"));
}

// §22.3: a readonly field passed by `ref` FROM WITHIN its own constructor
// stays accepted — it IS in its assignable context (POSITIVE control for
// the readonly-outside-context test above; kept here for contrast).
BEZEL_TEST(ref_out_operand_category_1682, out_to_readonly_field_inside_ctor_accepted_22_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public readonly V: int; "
        "  public static method Fill(out x: int) x := 7; end Fill; "
        "  public constructor() C.Fill(out this.V); end constructor; "
        "  public static method Main(): int "
        "    using var c := new C(); return c.V; "
        "  end Main; "
        "end C;"), 7);
}

// §22.3: a plain (non-`using`) owned local — never explicitly discharged
// via `using var` — is still owned state; a `ref` write through it bypasses
// the drop-on-overwrite/consume-site-freeze machinery. Pre-fix: compiled
// clean (only the unrelated §28.3 auto-drop WARNING fired, not an error).
//
// This is the OWNERSHIP-pass half of §22.3 (a live-lattice `lookupState`
// query in Ownership_Checks.cpp's checkRefOutOwnedOperand): the rejection is
// emitted by `checkOwnership`, which the deprecated JIT harness never carried
// into its diagnostics — so drive `parseAndCheckOwnership`, whose engine does.
BEZEL_TEST(ref_out_operand_category_1682, ref_to_plain_owned_local_rejected_22_3) {
    auto r = parseAndCheckOwnership(
        "module M; class Box public V: int := 0; end Box; "
        "class C "
        "  public static method Swallow(ref x: Box) x := new Box(); end Swallow; "
        "  public static method Main(): int "
        "    var b := new Box(); "
        "    C.Swallow(ref b); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(ownershipDiagContains(r, "§22.3"));
    BEZEL_EXPECT_TRUE(ownershipDiagContains(r, "owned local"));
}

// §22.3 / §28.10: a `using var` binding's disposal is pinned to scope exit;
// a `ref` write through it is invisible to that machinery. Pre-fix: compiled
// clean, no diagnostic.
BEZEL_TEST(ref_out_operand_category_1682, ref_to_using_var_local_rejected_22_3) {
    auto r = runAot(
        "module M; class Box public V: int := 0; end Box; "
        "class C "
        "  public static method Swallow(ref x: Box) x := new Box(); end Swallow; "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    C.Swallow(ref b); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.3"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "using var"));
}

// §22.3: an `owned` field is owned state — same alias-write hazard as an
// owned local. Pre-fix: compiled clean. Ownership-pass half (an
// `exprIsOwnedFieldRead` classification in checkRefOutOwnedOperand) — driven
// through `parseAndCheckOwnership` for the same reason as the owned-local
// test above.
BEZEL_TEST(ref_out_operand_category_1682, ref_to_owned_field_rejected_22_3) {
    auto r = parseAndCheckOwnership(
        "module M; class Box public V: int := 0; end Box; "
        "class Holder "
        "  private owned _box: Box; "
        "  public constructor() this._box := new Box(); end constructor; "
        "  public method Swallow(ref x: Box) x := new Box(); end Swallow; "
        "  public method Do() this.Swallow(ref this._box); end Do; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); h.Do(); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(ownershipDiagContains(r, "§22.3"));
    BEZEL_EXPECT_TRUE(ownershipDiagContains(r, "owned"));
}

// §22.3: an owning-collection indexer slot (`OwningArray<T>[i]`) is owned
// state — the spec's own named example ("M(ref pool[0]) on OwningArray
// rejected"). The rejection is the OWNERSHIP-pass half (an
// `indexerSetterTakesOwned` / `targetIsOwningInlineArraySlot` classification
// in checkRefOutOwnedOperand), so drive `parseAndCheckOwnership`.
//
// The "internal:" assertion below pins a HARNESS artifact, not a compiler
// bug: through the deprecated in-process JIT harness this program passes sema
// (the slot IS a structurally valid indexer target that sema admits) and —
// because that harness did not carry the ownership pass into its diagnostics —
// proceeds to codegen's lvalueAddr,
// which has no copy-in/copy-out bracketing for an accessor-mediated ref/out
// target (tracked at #1302) and emits "internal: lowerExpr returned null …
// #1386". `parseAndCheckOwnership` stops after the ownership pass — which
// rejects the slot cleanly with §22.3 BEFORE any codegen — so the internal
// message never arises. Verified against bezelc `-c` (full AOT pipeline): the
// §22.3 diagnostic is the ONLY error and no "internal:" appears, because the
// ownership error halts compilation before codegen.
BEZEL_TEST(ref_out_operand_category_1682, ref_to_owning_array_slot_rejected_22_3) {
    auto r = parseAndCheckOwnership(
        "module M; class Box public V: int := 0; end Box; "
        "class C "
        "  public static method Swallow(ref x: Box) x := new Box(); end Swallow; "
        "  public static method Main(): int "
        "    using var pool := new OwningArray<Box>(); "
        "    pool.Add(new Box()); "
        "    C.Swallow(ref pool[0]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(ownershipDiagContains(r, "§22.3"));
    BEZEL_EXPECT_FALSE(ownershipDiagContains(r, "internal:"));
}

// §22.3 / §22.12 rule 2: a mutable (non-readonly, non-`const`) static
// field's store semantics are defined for direct assignment only. Pre-fix:
// compiled clean.
BEZEL_TEST(ref_out_operand_category_1682, ref_to_mutable_static_rejected_22_3) {
    auto r = runAot(
        "module M; class C "
        "  public static Count: int := 0; "
        "  public static method Bump(ref x: int) x := x + 1; end Bump; "
        "  public static method Main(): int "
        "    C.Bump(ref C.Count); return C.Count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.3"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.12"));
}

// §22.3: a property with no setter is not a storage location at all.
// (Property is expression-bodied; per §23.2 (v0.214) `readonly` is no longer
// admitted on expression-bodied properties — the keyword has been dropped.)
BEZEL_TEST(ref_out_operand_category_1682, ref_to_no_setter_property_rejected_22_3) {
    auto r = runAot(
        "module M; class C "
        "  public property P: int => 5; "
        "  public static method Bump(ref x: int) x := x + 1; end Bump; "
        "  public method Do() C.Bump(ref this.P); end Do; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.3"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "no setter"));
}

// §22.3: a `const` member is a compile-time constant, not a storage
// location.
BEZEL_TEST(ref_out_operand_category_1682, ref_to_const_member_rejected_22_3) {
    auto r = runAot(
        "module M; class C "
        "  public const K: int := 5; "
        "  public static method Bump(ref x: int) x := x + 1; end Bump; "
        "  public static method Main(): int "
        "    C.Bump(ref C.K); return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.3"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "const"));
}

// =============================================================================
// POSITIVE — admitted categories compile clean (no §22.3 diagnostic) and,
// where the compiler already has working codegen for the category, run to
// the spec-traced correct value.
// =============================================================================

// §22.3: a mutable local variable is the canonical admitted category.
BEZEL_TEST(ref_out_operand_category_1682, ref_to_plain_local_accepted_22_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Inc(ref x: int) x := x + 1; end Inc; "
        "  public static method Main(): int "
        "    var n := 5; Inc(ref n); return n; "
        "  end Main; "
        "end C;"), 6);
}

// §22.3: a writable (non-readonly, non-owned) field is admitted.
BEZEL_TEST(ref_out_operand_category_1682, ref_to_writable_field_accepted_22_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class N public V: int := 0; end N; "
        "class C "
        "  public static method Inc(ref x: int) x := x + 1; end Inc; "
        "  public static method Main(): int "
        "    var n := new N(); n.V := 41; Inc(ref n.V); return n.V; "
        "  end Main; "
        "end C;"), 42);
}

// §22.3: an `out` local variable is likewise admitted.
BEZEL_TEST(ref_out_operand_category_1682, out_to_plain_local_accepted_22_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Get(out x: int) x := 42; end Get; "
        "  public static method Main(): int "
        "    var n := 0; Get(out n); return n; "
        "  end Main; "
        "end C;"), 42);
}

// §22.3: a `ref`/`out` parameter of the enclosing method, forwarded to a
// nested call, is an ordinary mutable storage location — not owned state —
// and stays admitted.
BEZEL_TEST(ref_out_operand_category_1682, ref_param_forwarded_accepted_22_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Swap(ref a: int, ref b: int) "
        "    var t := a; a := b; b := t; "
        "  end Swap; "
        "  public static method Main(): int "
        "    var x := 3; var y := 7; Swap(ref x, ref y); return x * 10 + y; "
        "  end Main; "
        "end C;"), 73);
}

// §22.3: a property WITH an accessible setter is an admitted category (no
// §22.3 diagnostic) — even though the compiler's copy-in/copy-out
// bracketing for accessor-mediated ref/out targets is not yet implemented
// (tracked separately at #1302; out of scope for the operand-category rule
// this test targets), so the compile as a whole still fails past sema. The
// assertion here is narrowly that §22.3 itself does not reject the operand.
BEZEL_TEST(ref_out_operand_category_1682, ref_to_settable_property_not_rejected_by_22_3) {
    auto r = runAot(
        "module M; class C "
        "  private _p: int := 0; "
        "  public property P: int get => this._p; set => this._p := value; end; "
        "  public static method Bump(ref x: int) x := x + 1; end Bump; "
        "  public method Do() C.Bump(ref this.P); end Do; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "§22.3"));
}

// §22.3: the inline declaration forms (`out var` / `out using var` /
// `out owned var`) always declare a FRESH local — there is no pre-existing-
// location question, so they are entirely unaffected by this rule.
BEZEL_TEST(ref_out_operand_category_1682, out_var_inline_form_unaffected_22_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method TryParse(out n: int): bool n := 9; return true; end TryParse; "
        "  public static method Main(): int "
        "    if C.TryParse(out var n) then return n; end; "
        "    return -1; "
        "  end Main; "
        "end C;"), 9);
}
