// Spec-derived runtime tests for §22.16's AUTO-PROPERTY setter row of the
// instrumentation set (v0.307, issue #4646 half 1).
//
// §22.16 "The instrumentation set — the invariant is evaluated at exactly
// these points, in checked-contracts mode":
//
//   "- **Entry and normal exit** of every `public` **setter** — block-form,
//      auto-property, indexer, `set(owned)` alike. (Auto-property assignments
//      inside constructors are backing-field stores, not accessor
//      invocations, so construction does not double-check.)"
//
// #4614(b) emitted that row at `lowerPropertySetValue` — the path a BLOCK-FORM
// setter's inlined body takes — and #4651 made the exported cross-module stub
// the same checked face. Neither can reach an AUTO-property: a same-module
// `c.P := v` on a non-virtual auto-property has no setter body at all, so
// `computedSetterOf` returns null, `lowerPropertySet` declines, and the store
// falls through to the generic `lvalueAddr` arm of `lowerStmt`'s AssignmentStmt
// lowering, landing directly in the backing field.
//
// MEASURED on this branch's HEAD before the fix, with `--checked-contracts`,
// on the issue's own repro (`c.k := -1; c.P := 7;` against `invariant(k > 0)`):
// 42, where §22.16 requires ContractViolationException (99). Both boundaries
// were measured independently and both gave 42. That is a SILENT UNDER-CHECK,
// the direction §22.16's observable-boundary property exists to prevent.
//
// ORACLE DISCIPLINE. The pre-fix behaviour is a WRONG VALUE, not a crash, so
// every row asserts the exact §22.16 verdict; `runChecked` collapses "did not
// terminate normally" to -999 so a regression that crashes cannot pass. The
// two boundaries are pinned SEPARATELY (rows 1 and 2) and each is ISOLATED —
// row 1's entry state satisfies the invariant, row 2's exit state satisfies it
// — so a fix emitting only one of them leaves the other red.
//
// THE OVER-CHECK IS THE EXPENSIVE FAILURE, and four rows exist to catch it.
// §22.16 exempts a `this`-receiver store ("A call whose receiver is `this` —
// explicit or implicit — from inside any instance member of the class or a
// subclass enters the member's unchecked face"), exempts the constructor's own
// auto-property stores by name, never instruments a non-public setter (§13.6
// accessor visibility), and never instruments a public FIELD store at all.
// Rows 4, 5, 8 and 9 pin each of those, and every one of them holds on BOTH
// sides of the fix: without them the instrumentation could be emitted
// unconditionally and turn the silent under-check into a loud over-check with
// nothing here to notice.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

int runChecked(const std::string& program) {
    auto r = bezel::test::runAotCheckedContracts(program);
    return r.ran ? r.exitCode : -999;
}

int runRelease(const std::string& program) {
    auto r = bezel::test::runAot(program, bezel::codegen::CodegenOptions{});
    return r.ran ? r.exitCode : -999;
}

// §22.16 + §28.2 together: the ordering row below needs BOTH checked modes.
int runCheckedBoth(const std::string& program) {
    bezel::codegen::CodegenOptions opts;
    opts.checkedContracts = true;   // §22.16
    opts.checkedDisposal = true;    // §28.2
    auto r = bezel::test::runAot(program, opts);
    return r.ran ? r.exitCode : -999;
}

// ★ Row 1 — the NORMAL-EXIT boundary, ISOLATED. The invariant reads the
// auto-property itself and HOLDS when the store is entered (P == 5); the store
// is what breaks it. Only an exit check can observe that. Pre-fix 42; §22.16
// requires 99.
const char* kAutoPropExitBoundary =
    "module S; class C "
    "  public property P: int := 1; "
    "  invariant(P > 0); "
    "  public constructor(v: int) P := v; end constructor; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(5); "
    "    try "
    "      c.P := 0 - 3; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// ★ Row 2 — the ENTRY boundary, ISOLATED. The instance is driven into a
// rejected state through a public FIELD (never an instrumentation point), and
// the auto-property store RESTORES the invariant: at entry `k == -1` and
// `P == 1`, so `k > 0 or P > 100` is false; at exit `P == 101`, so it is true
// again. Only an ENTRY check can observe this one. Pre-fix 42; §22.16 requires
// 99.
const char* kAutoPropEntryBoundary =
    "module S; class C "
    "  public k: int := 1; "
    "  public property P: int := 1; "
    "  invariant(k > 0 or P > 100); "
    "  public constructor(v: int) k := v; P := 1; end constructor; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(5); "
    "    try "
    "      c.k := 0 - 1; "
    "      c.P := 101; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// ★ Row 3 — the issue's own repro, character for character: an instance broken
// through a public field, then a conforming-looking auto-property store. The
// filed measurement was 42; §22.16 requires 99.
const char* kIssueRepro4646 =
    "module S; class C "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public property P: int := 1; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(5); "
    "    try "
    "      c.k := 0 - 1; "
    "      c.P := 7; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// Control — §22.16's `this`-reentrancy exemption governs the auto-property
// store exactly as it governs the block-form setter (#4614(b)) and the virtual
// one (#4613). `Rebuild` drives the instance through a state its own invariant
// rejects via its own auto-property and restores it before returning. 4 on
// BOTH sides of the fix.
const char* kThisReceiverAutoPropExempt =
    "module S; class C "
    "  public property P: int := 1; "
    "  invariant(P > 0); "
    "  public constructor(v: int) P := v; end constructor; "
    "  public method Rebuild(): int "
    "    this.P := 0 - 3; "
    "    this.P := 4; "
    "    return P; "
    "  end Rebuild; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(5); "
    "    try "
    "      r := c.Rebuild(); "
    "    catch ContractViolationException => r := 88; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// Control — §22.16's parenthetical, by name: "Auto-property assignments inside
// constructors are backing-field stores, not accessor invocations, so
// construction does not double-check." The constructor passes through a
// rejected state (`P := -3`) in BOTH the implicit and the explicit `this`
// spelling and restores it, so only the construction-exit check runs and it
// passes. 4 on BOTH sides of the fix.
const char* kConstructorAutoPropStoreExempt =
    "module S; class C "
    "  public property P: int := 1; "
    "  invariant(P > 0); "
    "  public constructor() "
    "    P := 0 - 3; "
    "    this.P := 4; "
    "  end constructor; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    try "
    "      using var c := new C(); "
    "      r := c.P; "
    "    catch ContractViolationException => r := 88; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// Control — a CONFORMING store is unobservable (§22.16's clauses are pure).
// 7 on both sides; pins that the fix does not disturb the happy path.
const char* kConformingAutoPropStore =
    "module S; class C "
    "  public property P: int := 1; "
    "  invariant(P > 0); "
    "  public constructor(v: int) P := v; end constructor; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    using var c := new C(5); "
    "    c.P := 7; "
    "    return c.P; "
    "  end Main; "
    "end MainS;";

// Control + its differential twin — §22.16's never-instrumented list names
// "non-public methods and SETTERS", and §13.6's accessor visibility is what
// makes `public property P: int private set;` one. The two programs are
// byte-identical but for `private set`, and BOTH reach the store on ANOTHER
// instance (`o.P := …`, not `this`), so the §22.16 `this`-reentrancy exemption
// is not what separates them — visibility is. The private row is 42 on BOTH
// sides of the fix; the public row is 42 pre-fix and 99 with it.
const char* kOtherInstanceAutoPropPrivateSet =
    "module S; class Cx "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public property P: int private set; "
    "  public method Poke(o: Cx): int "
    "    o.P := 3; "
    "    o.k := 0 - 1; "
    "    o.P := 4; "
    "    return 1; "
    "  end Poke; "
    "end Cx; "
    "class Mainx "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var a := new Cx(5); "
    "    using var b := new Cx(5); "
    "    try "
    "      r := a.Poke(b); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end Mainx;";

const char* kOtherInstanceAutoPropPublicSet =
    "module S; class Cx "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public property P: int; "
    "  public method Poke(o: Cx): int "
    "    o.P := 3; "
    "    o.k := 0 - 1; "
    "    o.P := 4; "
    "    return 1; "
    "  end Poke; "
    "end Cx; "
    "class Mainx "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var a := new Cx(5); "
    "    using var b := new Cx(5); "
    "    try "
    "      r := a.Poke(b); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end Mainx;";

// Control — a public FIELD store is not on §22.16's instrumentation set at
// all. The fix lives in the SAME generic-assignment arm of `lowerStmt` that
// lowers a field store, so this row is the guard against instrumenting the
// neighbour: `c.k := -1` alone must run to completion. 42 on BOTH sides.
const char* kPublicFieldStoreNotInstrumented =
    "module S; class C "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public property P: int := 1; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(5); "
    "    try "
    "      c.k := 0 - 1; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// §28.2 ordering fixture. The destructor zeroes the very auto-property its own
// `invariant(P > 0);` reads — §22.16 says disposal "legitimately tears the
// state down" — so a store on the DISPOSED instance is a point where §28.2's
// guard and §22.16's entry check disagree about which exception to raise.
// §28.2 wins: that is the order §22.16's public-METHOD row already uses
// (`emitFunctionBody` emits the disposed-flag test first), and the order
// #4651 measured and fixed for the cross-module setter stub. The program
// leaks by design (§28.2 quarantines a disposed object rather than freeing
// it).
const char* kDisposedAutoPropStore =
    "module S; class CD "
    "  public property P: int := 1; "
    "  invariant(P > 0); "
    "  public constructor(v: int) P := v; end constructor; "
    "  destructor P := 0; end destructor; "
    "end CD; "
    "class MainSD "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    var c := new CD(5); "
    "    c.Dispose(); "
    "    try "
    "      c.P := 7; "
    "      r := 42; "
    "    catch DisposedAccessException => r := 55; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainSD;";

}  // namespace

// ===========================================================================
// The two boundaries, pinned separately and each isolated from the other.
// ===========================================================================
BEZEL_TEST(invariant_auto_property_store_instrumented_4646,
           auto_property_normal_exit_boundary_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(kAutoPropExitBoundary));
}

BEZEL_TEST(invariant_auto_property_store_instrumented_4646,
           auto_property_entry_boundary_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(kAutoPropEntryBoundary));
}

BEZEL_TEST(invariant_auto_property_store_instrumented_4646,
           issue_repro_field_broken_then_auto_property_store_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(kIssueRepro4646));
}

// ===========================================================================
// The over-check guards. Every one of these holds on BOTH sides of the fix.
// ===========================================================================
BEZEL_TEST(invariant_auto_property_store_instrumented_4646,
           this_receiver_auto_property_store_is_exempt_spec_22_16) {
    BEZEL_EXPECT_EQ(4, runChecked(kThisReceiverAutoPropExempt));
}

BEZEL_TEST(invariant_auto_property_store_instrumented_4646,
           constructor_auto_property_store_is_exempt_spec_22_16) {
    BEZEL_EXPECT_EQ(4, runChecked(kConstructorAutoPropStoreExempt));
}

BEZEL_TEST(invariant_auto_property_store_instrumented_4646,
           conforming_auto_property_store_spec_22_16) {
    BEZEL_EXPECT_EQ(7, runChecked(kConformingAutoPropStore));
}

BEZEL_TEST(invariant_auto_property_store_instrumented_4646,
           non_public_auto_property_setter_never_instrumented_spec_13_6) {
    BEZEL_EXPECT_EQ(42, runChecked(kOtherInstanceAutoPropPrivateSet));
}

BEZEL_TEST(invariant_auto_property_store_instrumented_4646,
           public_auto_property_setter_on_another_instance_is_checked_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(kOtherInstanceAutoPropPublicSet));
}

BEZEL_TEST(invariant_auto_property_store_instrumented_4646,
           public_field_store_is_not_an_instrumentation_point_spec_22_16) {
    BEZEL_EXPECT_EQ(42, runChecked(kPublicFieldStoreNotInstrumented));
}

// §22.16 release default (#4386): no invariant is evaluated at all, so the
// violating store completes and `Main` returns 42. Without this row a fix
// that emitted the instrumentation unconditionally would pass.
BEZEL_TEST(invariant_auto_property_store_instrumented_4646,
           auto_property_unchecked_mode_control_spec_22_16) {
    BEZEL_EXPECT_EQ(42, runRelease(kAutoPropExitBoundary));
}

// §28.2 ORDERING, both checked modes on. Pre-fix this row is ALSO 55 (there
// was no invariant check at all), so it does not pin the bug — it pins the
// CHOICE the fix had to make, which was measured both ways: the entry check
// is emitted AFTER `lvalueAddr`, so §28.2's disposed guard (which `fieldAddr`
// emits internally) precedes it and DisposedAccessException wins, exactly as
// on the public-method row and on #4651's cross-module setter face.
BEZEL_TEST(invariant_auto_property_store_instrumented_4646,
           disposed_instance_raises_disposed_access_not_contract_spec_28_2) {
    BEZEL_EXPECT_EQ(55, runCheckedBoth(kDisposedAutoPropStore));
}
