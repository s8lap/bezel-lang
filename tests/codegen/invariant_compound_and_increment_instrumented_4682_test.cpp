// Spec-derived runtime tests for §22.16's AUTO-PROPERTY setter row reached
// through a COMPOUND assignment and through `++`/`--` (v0.307, issue #4682).
//
// AUTHORITY.
//
// §22.16, "Class invariants" — the instrumentation set:
//
//   "- **Entry and normal exit** of every `public` **setter** — block-form,
//      auto-property, indexer, `set(owned)` alike. (Auto-property assignments
//      inside constructors are backing-field stores, not accessor
//      invocations, so construction does not double-check.)"
//
// §6.6 makes a compound assignment a read-modify-write "equivalent to
// `x := x op e`", and §17.6 defines `x++` the same way ("`expr[i]++` desugars
// to a read-modify-write ... it is the indexer instance of the general
// compound-assignment rule of §6.6"). The WRITE half of each is therefore a
// setter invocation sitting on §22.16's boundary; the READ half is a getter,
// which §22.16 exempts by name ("Getters are exempt").
//
// THE DEFECT. #4646 instrumented the plain `AssignmentStmt` arm of
// `lowerStmt`. The compound arm and the `IncrementStmt` arm are siblings that
// reach the SAME generic `lvalueAddr` backing-field store and were outside
// that issue's scope, so neither evaluated the invariant at all.
//
// CONFIRMED on this branch's HEAD (92f2f29e, i.e. AFTER #4646/#4651/#4613),
// with `--checked-contracts`, on the issue's own two repros:
//
//     invariant(P > 0);  P == 1;  `c.P -= 4;`  -> 42   (§22.16 requires 99)
//     invariant(P < 10); P == 9;  `c.P++;`     -> 42   (§22.16 requires 99)
//
// while the byte-identical `c.P := -3;` on the same class already gave 99.
// That is a SILENT UNDER-CHECK, the direction §22.16's observable-boundary
// property exists to prevent.
//
// ORACLE DISCIPLINE. The pre-fix behaviour is a WRONG VALUE, not a crash, so
// every row asserts the exact §22.16 verdict; `runChecked` collapses "did not
// terminate normally" to -999 so a regression that crashes or fails to link
// cannot read as green. No row's verdict is an exit code a sanitizer abort
// could also produce (exit 1 is ASan's own abort code — never asserted here).
//
// THE TWO BOUNDARIES ARE PINNED SEPARATELY AND EACH IS ISOLATED (rows 1-4):
// the exit rows enter conforming and leave violating, the entry rows enter
// violating and leave conforming. A fix emitting only one boundary leaves the
// other red.
//
// ALL FOUR STORE PATHS OF THE COMPOUND ARM ARE PINNED (rows 1, 5, 6, 7). That
// arm is not one store: an auto-property can be typed to reach the §25.1/§25.2
// user-operator desugaring, the §4.6 string concat, the §6.4 decimal helper,
// or the CG31 built-in numeric store, and each has its own `return false`.
// Every one of the four measured 42 pre-fix.
//
// THE RECEIVER IS LOWERED ONCE (rows 8-9). A read-modify-write evaluates its
// lvalue chain twice in a naive implementation, which §6.6 forbids ("every
// constituent of the left-hand side's lvalue chain ... is evaluated exactly
// once"). ⚠ The issue's suggested spelling `f().P -= 1` is NOT VALID BEZEL —
// grammar A.6 requires an lvalue base to be an identifier or `this`, and the
// compiler rejects a call as an assignment target ("a method-call result
// cannot be the base of an assignment target", §6.6). The observable that IS
// spellable puts the side effect in an INDEX of the chain,
// `arr[ctr.Next()].P -= 6`, and each row folds the counter into its exit code
// so a double evaluation cannot hide behind a correct verdict.
//
// THE OVER-CHECK IS THE EXPENSIVE FAILURE, and eight rows exist to catch it
// (10-17, 19-20). §22.16 exempts a `this`-receiver store, exempts the
// constructor's own stores by name, never instruments a non-public setter
// (§13.6 accessor visibility), never instruments a public FIELD store at all,
// and evaluates nothing in release mode. Every one of those rows holds on BOTH
// sides of the fix.
//
// ⚠ A ROW'S FINAL STATE MUST SATISFY ITS GOVERNING INVARIANT. Rows 10-12 run
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

// §22.16 + §28.2 together: the ordering rows below need BOTH checked modes.
int runCheckedBoth(const std::string& program) {
    bezel::codegen::CodegenOptions opts;
    opts.checkedContracts = true;   // §22.16
    opts.checkedDisposal = true;    // §28.2
    auto r = bezel::test::runAot(program, opts);
    return r.ran ? r.exitCode : -999;
}

// ★ Row 1 — the issue's repro (a), which is also the compound NORMAL-EXIT
// boundary ISOLATED: the invariant reads the auto-property itself and HOLDS
// when the store is entered (P == 1); the store is what breaks it. Only an
// exit check can observe that. Measured 42 pre-fix; §22.16 requires 99.
const char* kCompoundExitBoundary =
    "module S; class C "
    "  public property P: int := 1; "
    "  invariant(P > 0); "
    "  public constructor(v: int) P := v; end constructor; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(1); "
    "    try "
    "      c.P -= 4; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// ★ Row 2 — the compound ENTRY boundary, ISOLATED. The instance is driven
// into a rejected state through a public FIELD (never an instrumentation
// point) and the compound store RESTORES the conjunction: at entry `k == -1`
// and `P == 1`, so `k > 0 or P > 100` is false; at exit `P == 101`, so it is
// true again. Only an ENTRY check can observe this one.
const char* kCompoundEntryBoundary =
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
    "      c.P += 100; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// ★ Row 3 — the issue's repro (b): `c.P++` as the §17.6 read-modify-write,
// NORMAL-EXIT boundary isolated (P == 9 satisfies `P < 10` at entry; the
// increment is what breaks it). Measured 42 pre-fix.
const char* kIncrementExitBoundary =
    "module S; class C "
    "  public property P: int := 1; "
    "  invariant(P < 10); "
    "  public constructor(v: int) P := v; end constructor; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(9); "
    "    try "
    "      c.P++; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// ★ Row 4 — the increment ENTRY boundary, ISOLATED. Same shape as row 2: the
// `++` RESTORES the conjunction (P goes 100 -> 101), so only an entry check
// can observe the broken pre-state.
const char* kIncrementEntryBoundary =
    "module S; class C "
    "  public k: int := 1; "
    "  public property P: int := 1; "
    "  invariant(k > 0 or P > 100); "
    "  public constructor(v: int) k := v; P := 100; end constructor; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(5); "
    "    try "
    "      c.k := 0 - 1; "
    "      c.P++; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// ★ Row 5 — the §6.4 DECIMAL store path of the compound arm. `decimal` is a
// struct-typed primitive routed through `emitDecimalArith`, a different
// `return false` from row 1's. Measured 42 pre-fix.
const char* kDecimalCompound =
    "module S; class C "
    "  public property P: decimal := 1m; "
    "  invariant(P > 0m); "
    "  public constructor(v: decimal) P := v; end constructor; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(5m); "
    "    try "
    "      c.P -= 8m; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// ★ Row 6 — the §4.6 STRING CONCAT store path (`s += t` is a concat, not an
// arithmetic op, and has its own store + release sequence). Measured 42
// pre-fix.
const char* kStringCompound =
    "module S; class C "
    "  public property P: string := \"a\"; "
    "  invariant(P.Length < 4); "
    "  public constructor(v: string) P := v; end constructor; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(\"ab\"); "
    "    try "
    "      c.P += \"cde\"; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// ★ Row 7 — the §25.1/§25.2 USER-OPERATOR desugaring path (`target :=
// operator_op_(target, value)`), a fourth `return false` again. Measured 42
// pre-fix.
const char* kUserOperatorCompound =
    "module S; "
    "record Money(Amount: int) "
    "  public static method operator -(a: Money, b: Money): Money => "
    "    new Money(a.Amount - b.Amount); "
    "end Money; "
    "class C "
    "  public property P: Money := new Money(1); "
    "  invariant(P.Amount > 0); "
    "  public constructor(v: int) P := new Money(v); end constructor; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(5); "
    "    try "
    "      c.P -= new Money(8); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// ★ Row 8 — §6.6's "evaluated exactly once", for the COMPOUND form. The
// counter advance sits in an INDEX of the lvalue chain (the only spelling
// grammar A.6 admits for a side-effecting constituent — see the header). The
// exit code folds both facts together: 90 for the §22.16 verdict plus
// `ctr.n`, so 91 means "violation raised AND the receiver chain ran once".
// A second lowering of the receiver would give 92; the pre-fix value is 43.
const char* kCompoundReceiverOnce =
    "module S; class C "
    "  public property P: int := 1; "
    "  invariant(P > 0); "
    "  public constructor(v: int) P := v; end constructor; "
    "end C; "
    "class Ctr "
    "  public n: int := 0; "
    "  public constructor() n := 0; end constructor; "
    "  public method Next(): int "
    "    n := n + 1; "
    "    return 0; "
    "  end Next; "
    "end Ctr; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var arr := new OwningArray<C>(); "
    "    arr.Add(new C(5)); "
    "    using var ctr := new Ctr(); "
    "    try "
    "      arr[ctr.Next()].P -= 6; "
    "      r := 42; "
    "    catch ContractViolationException => r := 90; "
    "    end; "
    "    return r + ctr.n; "
    "  end Main; "
    "end MainS;";

// ★ Row 9 — the same for the INCREMENT form (`--` from P == 1).
const char* kIncrementReceiverOnce =
    "module S; class C "
    "  public property P: int := 1; "
    "  invariant(P > 0); "
    "  public constructor(v: int) P := v; end constructor; "
    "end C; "
    "class Ctr "
    "  public n: int := 0; "
    "  public constructor() n := 0; end constructor; "
    "  public method Next(): int "
    "    n := n + 1; "
    "    return 0; "
    "  end Next; "
    "end Ctr; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var arr := new OwningArray<C>(); "
    "    arr.Add(new C(1)); "
    "    using var ctr := new Ctr(); "
    "    try "
    "      arr[ctr.Next()].P--; "
    "      r := 42; "
    "    catch ContractViolationException => r := 90; "
    "    end; "
    "    return r + ctr.n; "
    "  end Main; "
    "end MainS;";

// ★ Rows 8b/9b — the MEMBER-CHAIN spelling of the same property, which is the
// closest legal relative of the issue's `f().P -= 1`: `c.Self.P` whose
// mid-chain hop is a getter with an observable side effect. §6.6: "every
// constituent of the left-hand side's lvalue chain ... is evaluated exactly
// once". Same 90 + `calls` encoding as rows 8-9, so 91 means "violation
// raised AND the chain hop ran once"; a second lowering gives 92, and the
// pre-fix value is 43.
const char* kCompoundChainHopOnce =
    "module S; class C "
    "  public calls: int := 0; "
    "  public property P: int := 1; "
    "  invariant(P > 0); "
    "  public constructor(v: int) P := v; end constructor; "
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
    "    using var c := new C(5); "
    "    try "
    "      c.Self.P -= 6; "
    "      r := 42; "
    "    catch ContractViolationException => r := 90; "
    "    end; "
    "    return r + c.calls; "
    "  end Main; "
    "end MainS;";

const char* kIncrementChainHopOnce =
    "module S; class C "
    "  public calls: int := 0; "
    "  public property P: int := 1; "
    "  invariant(P > 0); "
    "  public constructor(v: int) P := v; end constructor; "
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
    "    using var c := new C(1); "
    "    try "
    "      c.Self.P--; "
    "      r := 42; "
    "    catch ContractViolationException => r := 90; "
    "    end; "
    "    return r + c.calls; "
    "  end Main; "
    "end MainS;";

// Control — §22.16's `this`-reentrancy exemption over a COMPOUND store.
// `Rebuild` drives its own instance through a state its invariant rejects and
// restores it before returning, so `Rebuild`'s own public-method exit check
// passes. 9 on BOTH sides of the fix.
const char* kThisReceiverCompoundExempt =
    "module S; class C "
    "  public property P: int := 1; "
    "  invariant(P > 0); "
    "  public constructor(v: int) P := v; end constructor; "
    "  public method Rebuild(): int "
    "    this.P -= 8; "
    "    this.P += 12; "
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

// Control — the same exemption over `++`/`--`. `Bump` passes through P == 0,
// which `invariant(P > 0)` rejects, and leaves P == 2. 2 on BOTH sides.
const char* kThisReceiverIncrementExempt =
    "module S; class C "
    "  public property P: int := 1; "
    "  invariant(P > 0); "
    "  public constructor(v: int) P := v; end constructor; "
    "  public method Bump(): int "
    "    this.P--; "
    "    this.P++; "
    "    this.P++; "
    "    return P; "
    "  end Bump; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(1); "
    "    try "
    "      r := c.Bump(); "
    "    catch ContractViolationException => r := 88; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// Control — §22.16's parenthetical, by name: "Auto-property assignments
// inside constructors are backing-field stores, not accessor invocations, so
// construction does not double-check." The constructor passes through P == -3
// via a bare compound store and through P == -2 via an explicit `this.P++`,
// and restores the conjunction, so only the construction-exit check runs and
// it passes. 4 on BOTH sides.
const char* kConstructorCompoundAndIncrementExempt =
    "module S; class C "
    "  public property P: int := 1; "
    "  invariant(P > 0); "
    "  public constructor() "
    "    P := 5; "
    "    P -= 8; "
    "    this.P++; "
    "    P += 6; "
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

// Control + its differential twin — §22.16's never-instrumented list names
// "non-public methods and SETTERS", and §13.6's accessor visibility is what
// makes `public property P: int private set;` one. The two programs are
// byte-identical but for `private set`, and BOTH compound-store on ANOTHER
// instance (`o.P -= 1`, not `this`), so the `this`-reentrancy exemption is not
// what separates them — visibility is. The private row is 42 on BOTH sides;
// the public row is 42 pre-fix and 99 with the fix.
const char* kOtherInstanceCompoundPrivateSet =
    "module S; class Cx "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public property P: int private set; "
    "  public method Poke(o: Cx): int "
    "    o.P -= 1; "
    "    o.k := 0 - 1; "
    "    o.P -= 1; "
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

const char* kOtherInstanceCompoundPublicSet =
    "module S; class Cx "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public property P: int; "
    "  public method Poke(o: Cx): int "
    "    o.P -= 1; "
    "    o.k := 0 - 1; "
    "    o.P -= 1; "
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

// Control — a public FIELD is not on §22.16's instrumentation set at all, and
// both new arms lower a field store through the same `lvalueAddr` they lower
// an auto-property store through. `c.k -= 8` and `c.k--` must both run to
// completion with the invariant left false. 42 on BOTH sides.
const char* kPublicFieldCompoundNotInstrumented =
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
    "      c.k -= 8; "
    "      c.k--; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// Control — a CONFORMING compound store and increment are unobservable
// (§22.16's clauses are pure). 4 on both sides; pins that the fix does not
// disturb the happy path.
const char* kConformingCompoundAndIncrement =
    "module S; class C "
    "  public property P: int := 1; "
    "  invariant(P > 0); "
    "  public constructor(v: int) P := v; end constructor; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    using var c := new C(5); "
    "    c.P -= 2; "
    "    c.P++; "
    "    return c.P; "
    "  end Main; "
    "end MainS;";

// §28.2 ORDERING fixtures. The destructor zeroes the very auto-property its
// own `invariant(P > 0);` reads — §22.16 says disposal "legitimately tears the
// state down" — so a compound store / decrement on the DISPOSED instance is a
// point where §28.2's guard and §22.16's entry check disagree about which
// exception to raise. §28.2 wins, which is the order the public-METHOD row
// uses and the order #4646 measured for the `:=` arm: the boundary check is
// emitted AFTER `lvalueAddr`, and `fieldAddr` emits the disposed guard
// internally. The programs leak by design (§28.2 quarantines a disposed
// object rather than freeing it).
const char* kDisposedCompound =
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
    "      c.P -= 1; "
    "      r := 42; "
    "    catch DisposedAccessException => r := 55; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainSD;";

const char* kDisposedIncrement =
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
    "      c.P--; "
    "      r := 42; "
    "    catch DisposedAccessException => r := 55; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainSD;";

}  // namespace

// ===========================================================================
// The two boundaries, pinned separately for each arm and each isolated.
// ===========================================================================
BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           compound_normal_exit_boundary_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(kCompoundExitBoundary));
}

BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           compound_entry_boundary_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(kCompoundEntryBoundary));
}

BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           increment_normal_exit_boundary_spec_17_6) {
    BEZEL_EXPECT_EQ(99, runChecked(kIncrementExitBoundary));
}

BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           increment_entry_boundary_spec_17_6) {
    BEZEL_EXPECT_EQ(99, runChecked(kIncrementEntryBoundary));
}

// ===========================================================================
// The compound arm's other three store paths — each its own `return false`.
// ===========================================================================
BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           decimal_compound_store_boundary_spec_6_4) {
    BEZEL_EXPECT_EQ(99, runChecked(kDecimalCompound));
}

BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           string_concat_compound_store_boundary_spec_4_6) {
    BEZEL_EXPECT_EQ(99, runChecked(kStringCompound));
}

BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           user_operator_compound_store_boundary_spec_25_1) {
    BEZEL_EXPECT_EQ(99, runChecked(kUserOperatorCompound));
}

// ===========================================================================
// §6.6 / §17.6 single evaluation of the lvalue chain.
// ===========================================================================
BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           compound_lvalue_chain_evaluated_once_spec_6_6) {
    BEZEL_EXPECT_EQ(91, runChecked(kCompoundReceiverOnce));
}

BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           increment_lvalue_chain_evaluated_once_spec_17_6) {
    BEZEL_EXPECT_EQ(91, runChecked(kIncrementReceiverOnce));
}

BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           compound_member_chain_hop_evaluated_once_spec_6_6) {
    BEZEL_EXPECT_EQ(91, runChecked(kCompoundChainHopOnce));
}

BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           increment_member_chain_hop_evaluated_once_spec_17_6) {
    BEZEL_EXPECT_EQ(91, runChecked(kIncrementChainHopOnce));
}

// ===========================================================================
// The over-check guards. Every one of these holds on BOTH sides of the fix.
// ===========================================================================
BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           this_receiver_compound_store_is_exempt_spec_22_16) {
    BEZEL_EXPECT_EQ(9, runChecked(kThisReceiverCompoundExempt));
}

BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           this_receiver_increment_is_exempt_spec_22_16) {
    BEZEL_EXPECT_EQ(2, runChecked(kThisReceiverIncrementExempt));
}

BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           constructor_compound_and_increment_are_exempt_spec_22_16) {
    BEZEL_EXPECT_EQ(4, runChecked(kConstructorCompoundAndIncrementExempt));
}

BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           non_public_compound_setter_never_instrumented_spec_13_6) {
    BEZEL_EXPECT_EQ(42, runChecked(kOtherInstanceCompoundPrivateSet));
}

BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           public_compound_setter_on_another_instance_is_checked_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(kOtherInstanceCompoundPublicSet));
}

BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           public_field_compound_and_decrement_not_instrumented_spec_22_16) {
    BEZEL_EXPECT_EQ(42, runChecked(kPublicFieldCompoundNotInstrumented));
}

BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           conforming_compound_and_increment_spec_22_16) {
    BEZEL_EXPECT_EQ(4, runChecked(kConformingCompoundAndIncrement));
}

// §22.16 release default (#4386): no invariant is evaluated at all, so the
// violating store completes and `Main` returns 42. Without these rows a fix
// that emitted the instrumentation unconditionally would pass.
BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           compound_unchecked_mode_control_spec_22_16) {
    BEZEL_EXPECT_EQ(42, runRelease(kCompoundExitBoundary));
}

BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           increment_unchecked_mode_control_spec_22_16) {
    BEZEL_EXPECT_EQ(42, runRelease(kIncrementExitBoundary));
}

// §28.2 ORDERING, both checked modes on. Pre-fix these rows are ALSO 55
// (there was no invariant check at all), so they do not pin the bug — they
// pin the CHOICE the fix had to make, which was measured: the boundary check
// is emitted AFTER `lvalueAddr`, so §28.2's disposed guard precedes it and
// DisposedAccessException wins, exactly as on the public-method row, on
// #4651's cross-module setter face, and on #4646's `:=` arm.
BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           disposed_instance_compound_raises_disposed_access_spec_28_2) {
    BEZEL_EXPECT_EQ(55, runCheckedBoth(kDisposedCompound));
}

BEZEL_TEST(invariant_compound_increment_instrumented_4682,
           disposed_instance_increment_raises_disposed_access_spec_28_2) {
    BEZEL_EXPECT_EQ(55, runCheckedBoth(kDisposedIncrement));
}
