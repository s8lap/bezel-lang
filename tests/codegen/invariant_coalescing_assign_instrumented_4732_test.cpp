// Spec-derived runtime tests for §22.16's AUTO-PROPERTY setter row reached
// through the NULL-COALESCING assignment `obj.P ??= e` (v0.307, issue #4732).
//
// AUTHORITY.
//
// §22.16, "Class invariants" — the instrumentation set, verbatim:
//
//   "- **Entry and normal exit** of every `public` **setter** — block-form,
//      auto-property, indexer, `set(owned)` alike. (Auto-property assignments
//      inside constructors are backing-field stores, not accessor
//      invocations, so construction does not double-check.)"
//
// §6.6 on this exact spelling, verbatim:
//
//   "**`??=`** is not a read-modify-write: `x ??= e` assigns `e` only when
//    `x`'s current value is null, with the typing and ownership rules of
//    §7.7. The LHS chain is evaluated once as above, the getter once; `e` is
//    evaluated — and the setter invoked — only when the [assignment actually
//    occurs]."
//
// Those two sentences fix BOTH halves of this file's contract. "the setter
// invoked" makes the occurring store a setter invocation, so §22.16 owes it an
// entry and a normal-exit check; "only when" makes the short-circuit path no
// setter invocation at all, so instrumenting it would be an OVER-check. Every
// row below is one or the other.
//
// THE DEFECT. `lowerStmt`'s AssignmentStmt lowering has four sibling write
// arms and the §22.16 auto-property boundary reached three of them: `:=`
// (#4646), `OP=` and `++`/`--` (#4682). The `??=` arm is deliberately
// self-contained — #2905 routes it around the whole compound chain, because
// `lowerPropertyCompound` would evaluate `e` unconditionally and §6.6 forbids
// that — and it called `lvalueAddr` WITHOUT the `invRecv` out-parameter #4646
// added, stored into the backing field and returned before either instrumented
// arm was reached. It evaluated nothing.
//
// CONFIRMED on this branch's HEAD (6221ed38, i.e. AFTER #4646/#4651/#4682),
// with `--checked-contracts`, on the issue's own repro:
//
//     invariant(P = null);  P == null;  `c.P ??= 7;`  -> 42  (§22.16: 99)
//
// while the byte-identical `c.P := 7;` on the same class already gave 99.
// That is a SILENT UNDER-CHECK, the direction §22.16's observable-boundary
// property exists to prevent.
//
// WHY THIS ARM CARRIES MORE THAN ITS SHARE. §6.6 REJECTS every compound
// assignment form and `++`/`--` on a nullable operand and RETAINS `??=`. So
// for a `T?` auto-property `??=` is the only write spelling besides `:=`, and
// this arm was the whole of §22.16's setter row for it — which is why rows 1-3
// are typed `int?` and `string?` rather than `int`.
//
// ORACLE DISCIPLINE. The pre-fix behaviour is a WRONG VALUE, not a crash, so
// every row asserts the exact §22.16 verdict; `runChecked` collapses "did not
// terminate normally" to -999 so a regression that crashes or fails to link
// cannot read as green. No row's verdict is an exit code a sanitizer abort
// could also produce (exit 1 is ASan's own abort code — never asserted here),
// and no verdict exceeds 255 (`runAot`'s exitCode is WEXITSTATUS-masked).
//
// THE TWO BOUNDARIES ARE PINNED SEPARATELY AND EACH IS ISOLATED (rows 1-2):
// the exit row enters conforming and leaves violating, the entry row enters
// violating and leaves conforming. A fix emitting only one boundary leaves the
// other red.
//
// THE OVER-CHECK IS THE EXPENSIVE FAILURE, and seven rows exist to catch it
// (4, 6-11). The §6.6 short-circuit row (4) is the one this arm's shape makes
// specific: a check hoisted out of the `coaleq.do` branch — the obvious way to
// write this fix — passes rows 1-3 and fails row 4, because §6.6 makes a
// non-null LHS no setter invocation and therefore no boundary. Row 4 also
// folds the property's value into its exit code, so "no check" cannot be
// bought by also dropping the store.
//
// ⚠ A ROW'S FINAL STATE MUST SATISFY ITS GOVERNING INVARIANT. Rows 6-7 run
// inside a public method or a constructor, which carry their OWN §22.16 exit
// check; a `this`-exempt row that leaves the instance violating faults at that
// check instead and would read as an over-check that is not one. Each of those
// rows therefore restores the conjunction before returning.

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

// ★ Row 1 — the issue's repro, which is also the `??=` NORMAL-EXIT boundary
// ISOLATED: the invariant reads the auto-property itself and HOLDS when the
// store is entered (P == null); the store is what breaks it. Only an exit
// check can observe that. Measured 42 pre-fix; §22.16 requires 99.
const char* kCoalesceExitBoundary =
    "module S; class C "
    "  public property P: int? := null; "
    "  invariant(P = null); "
    "  public constructor() P := null; end constructor; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(); "
    "    try "
    "      c.P ??= 7; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// ★ Row 2 — the `??=` ENTRY boundary, ISOLATED. The instance is driven into a
// rejected state through a public FIELD (never an instrumentation point —
// §22.16 names methods, setters and construction exit only) and the `??=`
// store RESTORES the conjunction: at entry `k == -1` and `P == null`, so
// `k > 0 or P <> null` is false; at exit `P == 7`, so it is true again. Only
// an ENTRY check can observe this one.
const char* kCoalesceEntryBoundary =
    "module S; class C "
    "  public k: int := 1; "
    "  public property P: int? := null; "
    "  invariant(k > 0 or P <> null); "
    "  public constructor(v: int) k := v; P := null; end constructor; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(5); "
    "    try "
    "      c.k := 0 - 1; "
    "      c.P ??= 7; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// ★ Row 3 — the `string?` store path. §4.6/§28.9 make a class string slot take
// an independent `+1` on this arm (`emitStringRetain` before the store), which
// is a different store sequence from row 1's scalar one and has its own
// ordering question: the ENTRY check must precede the retain, or a violating
// entry state leaves a retained string that is never stored (a leak). Measured
// 42 pre-fix.
const char* kCoalesceStringExitBoundary =
    "module S; class C "
    "  public property P: string? := null; "
    "  invariant(P = null); "
    "  public constructor() P := null; end constructor; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(); "
    "    try "
    "      c.P ??= \"abc\"; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// ★ Row 4 — §6.6's SHORT-CIRCUIT, which is this arm's own over-check trap.
// `P` is already non-null, so per §6.6 "`e` is evaluated — and the setter
// invoked — only when the assignment actually occurs" there is NO setter
// invocation and therefore NO §22.16 boundary, even though the conjunction is
// false at that point (`k == -1`). A fix that hoists the check above the
// `coaleq.do` branch passes rows 1-3 and turns this row from 47 into 99.
//
// The exit code folds the property's value in (42 + `P`), so "no check" cannot
// be bought by also dropping the store: 47 means "no violation raised AND `P`
// is still the 5 it held". 52 would mean the RHS was stored anyway.
// `c.k := 1` restores the conjunction before the `using` scope ends.
const char* kCoalesceNonNullShortCircuitNoCheck =
    "module S; class C "
    "  public k: int := 1; "
    "  public property P: int? := null; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; P := null; end constructor; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(5); "
    "    c.P := 5; "
    "    c.k := 0 - 1; "
    "    try "
    "      c.P ??= 10; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    c.k := 1; "
    "    return r + (c.P ?? 0); "
    "  end Main; "
    "end MainS;";

// ★ Row 5 — §6.6's "The LHS chain is evaluated once". The mid-chain hop is a
// getter with an observable side effect, so a second lowering of the receiver
// is countable. The exit code folds both facts together: 90 for the §22.16
// verdict plus `c.calls`, so 91 means "violation raised AND the chain hop ran
// once". A second lowering would give 92; the pre-fix value is 43.
const char* kCoalesceChainHopOnce =
    "module S; class C "
    "  public calls: int := 0; "
    "  public property P: int? := null; "
    "  invariant(P = null); "
    "  public constructor() P := null; end constructor; "
    "  public property Self: C "
    "    get => do "
    "      calls := calls + 1; "
    "      return this; "
    "    end; "
    "  end Self; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(); "
    "    try "
    "      c.Self.P ??= 7; "
    "      r := 42; "
    "    catch ContractViolationException => r := 90; "
    "    end; "
    "    return r + c.calls; "
    "  end Main; "
    "end MainS;";

// Control — §22.16's `this`-reentrancy exemption over a `??=` store.
// `Rebuild` drives its own instance through a state its invariant rejects and
// restores it before returning, so `Rebuild`'s own public-method exit check
// passes. 7 on BOTH sides of the fix.
const char* kCoalesceThisReceiverExempt =
    "module S; class C "
    "  public property P: int? := null; "
    "  invariant(P = null); "
    "  public constructor() P := null; end constructor; "
    "  public method Rebuild(): int "
    "    this.P ??= 7; "
    "    var got: int := this.P ?? 0; "
    "    this.P := null; "
    "    return got; "
    "  end Rebuild; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(); "
    "    try "
    "      r := c.Rebuild(); "
    "    catch ContractViolationException => r := 88; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// Control — §22.16's parenthetical names this one: "Auto-property assignments
// inside constructors are backing-field stores, not accessor invocations, so
// construction does not double-check." The constructor passes through the
// state `invariant(P = null)` rejects and restores it before construction
// exit. 5 on BOTH sides.
const char* kCoalesceConstructorExempt =
    "module S; class C "
    "  public property P: int? := null; "
    "  invariant(P = null); "
    "  public constructor() "
    "    P ??= 7; "
    "    P := null; "
    "  end constructor; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(); "
    "    try "
    "      r := 5; "
    "    catch ContractViolationException => r := 88; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// Control — a public FIELD is not on §22.16's instrumentation set at all, and
// this arm lowers a field store through the same `lvalueAddr` it lowers an
// auto-property store through. `c.f ??= 7` must run to completion with the
// invariant left false. The exit code folds `f` in (42 + 7 = 49) so the row
// also pins that the store still happens. 49 on BOTH sides.
const char* kCoalescePublicFieldNotInstrumented =
    "module S; class C "
    "  public f: int? := null; "
    "  invariant(f = null); "
    "  public constructor() f := null; end constructor; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(); "
    "    try "
    "      c.f ??= 7; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r + (c.f ?? 0); "
    "  end Main; "
    "end MainS;";

// Control + its differential twin — §22.16's never-instrumented list names
// "non-public methods and setters", and §13.6's accessor visibility is what
// makes `public property P: int? private set := null;` one. The two
// programs are identical but for that clause, and BOTH `??=`-store on ANOTHER
// instance (`o.P ??= 7`, not `this`), so the `this`-reentrancy exemption is
// not what separates them — visibility is. The private row is 42 on BOTH
// sides; the public row is 42 pre-fix and 99 with the fix.
const char* kOtherInstanceCoalescePrivateSet =
    "module S; class Cx "
    "  public property P: int? private set := null; "
    "  invariant(P = null); "
    "  public constructor() P := null; end constructor; "
    "  public method Poke(o: Cx): int "
    "    o.P ??= 7; "
    "    return 1; "
    "  end Poke; "
    "end Cx; "
    "class Mainx "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var a := new Cx(); "
    "    using var b := new Cx(); "
    "    try "
    "      r := a.Poke(b); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end Mainx;";

const char* kOtherInstanceCoalescePublicSet =
    "module S; class Cx "
    "  public property P: int? := null; "
    "  invariant(P = null); "
    "  public constructor() P := null; end constructor; "
    "  public method Poke(o: Cx): int "
    "    o.P ??= 7; "
    "    return 1; "
    "  end Poke; "
    "end Cx; "
    "class Mainx "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var a := new Cx(); "
    "    using var b := new Cx(); "
    "    try "
    "      r := a.Poke(b); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end Mainx;";

// Control — a CONFORMING `??=` store is unobservable: the check evaluates and
// passes, and the program keeps its value. 7 on BOTH sides.
const char* kCoalesceConformingStore =
    "module S; class C "
    "  public k: int := 1; "
    "  public property P: int? := null; "
    "  invariant(k > 0); "
    "  public constructor() k := 1; P := null; end constructor; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(); "
    "    try "
    "      c.P ??= 7; "
    "      r := c.P ?? 0; "
    "    catch ContractViolationException => r := 88; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// §28.2 ORDERING, both checked modes on. Pre-fix this row is ALSO 55 (there
// was no invariant check at all), so it does not pin the bug — it pins the
// CHOICE the fix had to make: the boundary check is emitted AFTER `lvalueAddr`,
// so §28.2's disposed guard precedes it and DisposedAccessException wins,
// exactly as on #4646's `:=` arm and #4682's compound one.
const char* kDisposedCoalesce =
    "module S; class CD "
    "  public property P: int? := null; "
    "  invariant(P <> null); "
    "  public constructor() P := 1; end constructor; "
    "  destructor P := null; end destructor; "
    "end CD; "
    "class MainSD "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    var c := new CD(); "
    "    c.Dispose(); "
    "    try "
    "      c.P ??= 7; "
    "      r := 42; "
    "    catch DisposedAccessException => r := 55; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainSD;";

}  // namespace

// ── §22.16's two boundaries, each isolated ─────────────────────────────────

BEZEL_TEST(invariant_coalescing_assign_instrumented_4732,
           coalesce_store_exit_boundary_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(kCoalesceExitBoundary));
}

BEZEL_TEST(invariant_coalescing_assign_instrumented_4732,
           coalesce_store_entry_boundary_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(kCoalesceEntryBoundary));
}

BEZEL_TEST(invariant_coalescing_assign_instrumented_4732,
           coalesce_string_store_exit_boundary_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(kCoalesceStringExitBoundary));
}

// ── §6.6's short-circuit is NOT a setter invocation ────────────────────────

BEZEL_TEST(invariant_coalescing_assign_instrumented_4732,
           non_null_lhs_short_circuits_and_is_unchecked_spec_6_6) {
    BEZEL_EXPECT_EQ(47, runChecked(kCoalesceNonNullShortCircuitNoCheck));
}

// ── §6.6's "LHS chain evaluated once" ──────────────────────────────────────

BEZEL_TEST(invariant_coalescing_assign_instrumented_4732,
           coalesce_chain_hop_lowered_once_spec_6_6) {
    BEZEL_EXPECT_EQ(91, runChecked(kCoalesceChainHopOnce));
}

// ── §22.16's exemptions, all unchanged by the fix ──────────────────────────

BEZEL_TEST(invariant_coalescing_assign_instrumented_4732,
           this_receiver_coalesce_exempt_spec_22_16) {
    BEZEL_EXPECT_EQ(7, runChecked(kCoalesceThisReceiverExempt));
}

BEZEL_TEST(invariant_coalescing_assign_instrumented_4732,
           constructor_coalesce_exempt_spec_22_16) {
    BEZEL_EXPECT_EQ(5, runChecked(kCoalesceConstructorExempt));
}

BEZEL_TEST(invariant_coalescing_assign_instrumented_4732,
           public_field_coalesce_not_instrumented_spec_22_16) {
    BEZEL_EXPECT_EQ(49, runChecked(kCoalescePublicFieldNotInstrumented));
}

BEZEL_TEST(invariant_coalescing_assign_instrumented_4732,
           private_set_other_instance_coalesce_unchecked_spec_13_6) {
    BEZEL_EXPECT_EQ(42, runChecked(kOtherInstanceCoalescePrivateSet));
}

BEZEL_TEST(invariant_coalescing_assign_instrumented_4732,
           public_set_other_instance_coalesce_checked_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(kOtherInstanceCoalescePublicSet));
}

BEZEL_TEST(invariant_coalescing_assign_instrumented_4732,
           conforming_coalesce_store_unobservable_spec_22_16) {
    BEZEL_EXPECT_EQ(7, runChecked(kCoalesceConformingStore));
}

// ── §22.16's release default: nothing is emitted ───────────────────────────

BEZEL_TEST(invariant_coalescing_assign_instrumented_4732,
           coalesce_unchecked_mode_control_spec_22_16) {
    BEZEL_EXPECT_EQ(42, runRelease(kCoalesceExitBoundary));
}

// ── §28.2 ordering ─────────────────────────────────────────────────────────

BEZEL_TEST(invariant_coalescing_assign_instrumented_4732,
           disposed_instance_coalesce_raises_disposed_access_spec_28_2) {
    BEZEL_EXPECT_EQ(55, runCheckedBoth(kDisposedCoalesce));
}
