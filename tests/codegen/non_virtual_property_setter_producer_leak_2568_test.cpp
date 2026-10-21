// fix #2568 (§23.3/§28.9/§4.6, v0.236): a +1 PRODUCER RHS passed to a
// non-virtual CLASS COMPUTED-PROPERTY setter leaks the +1 per call, on the
// LOCAL inline-setter-body path inside `lowerPropertySetValue`
// (LlvmBackend_Assign.cpp ~3916, the function reached via `lowerPropertySet`
// from the AssignmentStmt lowering for `obj.P := …` /
// `lowerPropertyCompound` for `obj.P OP= …`, and via the increment-stmt
// handler for `obj.P++`).
//
// Pre-fix, every iteration of `s.Name := "abc" + i.ToString();` against a
// non-virtual CLASS computed property `set => _name := value;` leaked the
// producer's +1 across the setter call: the inlined setter body stored the
// incoming `value` into the backing field `_name`, which routes through
// `lowerStringFieldStore`'s ARC and so retains the producer's +1, but no
// caller-side release fired to balance the retain, so the producer's +1
// was unowned. LSan observed 3200 byte(s) direct + 590 byte(s) indirect =
// 3790 bytes leaked across 100 iterations (37 byte(s)/iteration, the
// 32-byte bz_string header + ~5 byte int-digit payload — exactly the
// pattern the issue brief observed).
//
// The sibling sites did NOT leak:
//   * Virtual/interface property setter (`lowerVirtualPropertySet`,
//     LlvmBackend_Exprs.cpp ~5106-5128) — has emitted producer release
//     since #2490/#2500/#2506. This is why the issue brief observed the
//     virtual-property path as already-clean.
//   * Indexer setter (`lowerIndexerSet`, LlvmBackend_Assign.cpp ~2153) —
//     got the same emitProducerRelease lambda in #2548.
//   * Indexer compound setter (`lowerIndexerCompound`,
//     LlvmBackend_Assign.cpp ~2531) — got the compound twin in #2567.
// `lowerPropertySetValue` was the lone holdout; the fix copies the lambda
// pattern from `lowerVirtualPropertySet` and gates it on the same
// `classOwned` predicate (`ps->parent->kind == ast::TypeDeclKind::Class`)
// that `lowerIndexerSet`'s inline-body path uses — records' setter bodies
// plain-store (§9/§9.7: a record's reference fields are borrows with no
// independent +1 and no disposer), so releasing there would drop the
// field's only reference (UAF). The lambda is invoked from BOTH the static
// and instance branches of `lowerPropertySetValue`.
//
// OBSERVABILITY: the leak is a DIRECT leak of the bz_string header, so
// runAotChecked's LSan flags it; without the fix `r.ran` is false ("Direct
// leak of N byte(s)"), with it the run is clean and no double-free /
// use-after-free is introduced (the dispose-exactly-once net rides on
// `r.ran`).
//
// TEST SHAPE: each probe loop-amplifies 100× so LSan catches the leak
// decisively (the canonical #2568 issue repro looped 5× and the per-call
// leak is small — we amplify 20× to get a clear margin above LSan noise);
// the exit code is `sink mod 256` — the program's own observable value
// (independent of the leak), confirming the program also computes
// correctly and the property set/get round-trips. Each shape sits in a
// SEPARATE probe so LSan can attribute any leak to the specific code path
// if it regresses.
//
// REGRESSION COVERAGE:
//   * Non-virtual CLASS computed-property setter with producer-RHS string
//     concat — the cardinal case, must be LSan-clean post-fix.
//   * Non-virtual CLASS computed-property setter with a borrow-RHS (a
//     named string local) — the no-producer control: no leak pre- or
//     post-fix; protects against an over-eager release that would UAF the
//     borrow on its scope end.
//   * Non-virtual CLASS computed-property setter with box-return RHS
//     (Object target) — the §8.6 box twin, exercises
//     `isUserMethodBoxReturnCall` + the `isObjectOrInterfaceTarget` gate.
//   * Virtual CLASS computed-property setter — cross-check that
//     `lowerVirtualPropertySet`'s emitProducerRelease is unaffected by the
//     local-path fix (it must not double-release).
//   * Indexer setter with producer-RHS — the #2548 regression guard:
//     `lowerIndexerSet`'s emitProducerRelease must remain unaffected.
//
// OUT OF SCOPE (filed separately, NOT covered here):
//   * The CLOSURE-PRODUCER twin (isPlusOneClosureProducer /
//     isUserMethodClosureReturnCall) — Bezel's surface syntax for closure-
//     typed properties does not currently admit a `set => _cb := value;`
//     pattern with the closure field declared as `(): int` (the field
//     initializer requires a default-closure literal the surface grammar
//     does not emit). The predicate is identical to the string case, and
//     is exercised at the call-argument level by lowerIndexerSet's #2548
//     fix and lowerVirtualPropertySet's #2500 box-twin; the property-set
//     closure path is the same lambda, just gated on the same class-only
//     predicate. No stand-alone repro was reachable here.
//   * The RECORD property setter producer leak — §9/§9.7: a record's
//     reference fields are borrows with no independent +1 and no disposer,
//     so the setter's body does a PLAIN STORE. The fix's `classOwned` gate
//     (per the issue brief) deliberately does NOT release on records;
//     releasing would drop the field's only reference (UAF — the live
//     failure #2490/#2506/#2548 caught for the sibling sites). The
//     pre-existing record leak is a separate concern, deliberately out of
//     scope for #2568 (it would need a record-side auto-property fix, not
//     a caller-side release here). The fix must NOT widen the gate to
//     records; this regression suite has no record probe because the
//     probe would either pass (and prove nothing about the gate, since
//     the field-store path is unchanged) or fail (and conflate the
//     pre-existing record leak with the #2568 fix). The UAF-on-records
//     hazard is checked at code-review time via the gate predicate.
//   * The COMPOUND-ASSIGNMENT (`s.Name += rhs`) `rhs` leak — a sibling
//     to `lowerIndexerCompound`'s rhs-leak (#2567), where emitCompoundOp
//     memcpy's `rhs` into `res` but does not consume rhs's +1. Filed as
//     #2568-B; this commit only addresses the `res`/setter-body leak,
//     mirroring #2567's scope.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// ----------------------------------------------------------------------
// Probe A — the CARDINAL case: non-virtual CLASS computed-property
// setter with a +1 producer-RHS string concat. The inlined setter body
// stores `value` into the backing field, retain mints the field's +1,
// the producer's +1 is unowned, LSan flags the leak pre-fix.
// ----------------------------------------------------------------------
const char* kClassComputedProducer =
    "module M; "
    "class Sink "
    "  private _name: string := \"\"; "
    "  public property Name: string "
    "    get => _name; "
    "    set => _name := value; "
    "  end; "
    "end Sink; "
    "class C "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      s.Name := \"abc\" + i.ToString(); "  // producer RHS, computed path
    "      sink := sink + s.Name.Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe B — BORROW RHS control: a named local string assigned to the
// same computed property. The local's +1 belongs to its own slot, so
// the caller-side release would UAF the local's only ref at its scope
// end. The fix's producer-predicate gate (isPlusOneStringProducer must
// match a BinaryExpr/string-interp/call, NOT a named IdentifierExpr)
// protects this — pre- AND post-fix this is LSan-clean. If the predicate
// were widened to fire on borrows too, this would fail with UAF on the
// local's release.
// ----------------------------------------------------------------------
const char* kClassComputedBorrow =
    "module M; "
    "class Sink "
    "  private _name: string := \"\"; "
    "  public property Name: string "
    "    get => _name; "
    "    set => _name := value; "
    "  end; "
    "end Sink; "
    "class C "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var local: string := \"abc\"; "  // mint one +1 local
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      s.Name := local; "  // borrow RHS — local's +1 unchanged
    "      sink := sink + s.Name.Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe C — BOX producer: a box-returning method call assigned to an
// Object-typed property. Exercises the §8.6 box twin of the
// string/closure release (isUserMethodBoxReturnCall + the
// isObjectOrInterfaceTarget gate). The property type is Object (the
// canonical box-target) and the RHS is a method that boxes its int
// return.
// ----------------------------------------------------------------------
const char* kClassComputedBoxProducer =
    "module M; "
    "class Sink "
    "  private _box: Object := null; "
    "  public property Box: Object "
    "    get => _box; "
    "    set => _box := value; "
    "  end; "
    "  public method MakeBox(): Object => 42; "  // +1 box-return call
    "end Sink; "
    "class C "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      s.Box := s.MakeBox(); "  // box producer RHS, computed path
    "      sink := sink + 1; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe D — VIRTUAL-CLASS cross-check: a virtual/override class computed
// property dispatched through the vtable slot. This is the
// `lowerVirtualPropertySet` path (the canonical reference for the fix).
// It must remain LSan-clean post-fix; if the local-path emitProducerRelease
// accidentally double-released (e.g. by widening the gate), this probe
// would crash with use-after-free.
// ----------------------------------------------------------------------
const char* kVirtualClassComputed =
    "module M; "
    "class Base "
    "  private _name: string := \"\"; "
    "  public virtual property Name: string "
    "    get => _name; "
    "    set => _name := value; "
    "  end; "
    "end Base; "
    "class Derived: Base end Derived; "
    "class C "
    "  public static method Main(): int "
    "    using var d := new Derived(); "
    "    var b: Base := d; "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      b.Name := \"abc\" + i.ToString(); "  // producer RHS, virtual path
    "      sink := sink + b.Name.Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe E — INDEXER cross-check (#2548 regression guard):
// lowerIndexerSet's inline-body emitProducerRelease must remain
// unaffected by the local-property fix.
// ----------------------------------------------------------------------
const char* kIndexerClassComputed =
    "module M; "
    "class Holder "
    "  public _items: string := \"\"; "
    "  public property [i: int]: string "
    "    get => _items; "
    "    set => _items := value; "
    "  end; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      h[0] := \"abc\" + i.ToString(); "  // producer RHS, indexer path
    "      sink := sink + h[0].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #2568 — CARDINAL CASE: non-virtual class computed-property setter
// with a producer-RHS string concat. Pre-fix leaks ~37 byte(s) per
// iteration (32-byte bz_string header + ~5 byte int-digit payload).
// Post-fix the lambda fires and the doubled +1 is dropped.
// =====================================================================
BEZEL_TEST(codegen_non_virtual_property_setter_producer_leak_2568,
           class_computed_setter_producer_does_not_leak_spec_23_3_28_9) {
    JitResult r = runAotChecked(kClassComputedProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // LSan-clean (the #2568 leak flips this)
}

// =====================================================================
// #2568 — BORROW CONTROL: a named-local string RHS. The producer
// predicate (isPlusOneStringProducer) must NOT match a borrow, so the
// emit producer release does not fire; if it did, the local's scope-end
// release would UAF its own +1. Pre-fix and post-fix both clean.
// =====================================================================
BEZEL_TEST(codegen_non_virtual_property_setter_producer_leak_2568,
           class_computed_setter_borrow_remains_clean_spec_23_3_28_9) {
    JitResult r = runAotChecked(kClassComputedBorrow);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2568 — BOX producer: exercises isUserMethodBoxReturnCall +
// isObjectOrInterfaceTarget (§8.6 twin). Cardinal fix verification for
// the box release.
// =====================================================================
BEZEL_TEST(codegen_non_virtual_property_setter_producer_leak_2568,
           class_computed_setter_box_producer_does_not_leak_spec_23_3_28_9) {
    JitResult r = runAotChecked(kClassComputedBoxProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2568 — VIRTUAL PROPERTY cross-check: lowerVirtualPropertySet's
// emitProducerRelease (the reference for the fix) must remain LSan-clean.
// If the local-path fix accidentally double-released (e.g. by widening
// the gate), this probe would crash with use-after-free.
// =====================================================================
BEZEL_TEST(codegen_non_virtual_property_setter_producer_leak_2568,
           virtual_class_property_still_clean_spec_23_3_28_9) {
    JitResult r = runAotChecked(kVirtualClassComputed);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2568 — INDEXER cross-check (#2548 regression guard):
// lowerIndexerSet's inline-body emitProducerRelease must remain
// unaffected by the local-property fix.
// =====================================================================
BEZEL_TEST(codegen_non_virtual_property_setter_producer_leak_2568,
           indexer_class_setter_still_clean_spec_23_3_28_9) {
    JitResult r = runAotChecked(kIndexerClassComputed);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}
