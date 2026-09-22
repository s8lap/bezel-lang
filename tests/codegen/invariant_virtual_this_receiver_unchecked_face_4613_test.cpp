// Spec-derived runtime tests for the VIRTUAL half of §22.16's `this`-
// reentrancy exemption (v0.306, issue #4613 — the residual #4597 left open).
//
// §22.16 "The visibility boundary and the `this`-reentrancy exemption":
//
//   "Every instrumented public member compiles to a checked face (entry
//    checks → unchecked face → exit checks) and an unchecked face (the body);
//    call sites with syntactic receiver `this` bind to the unchecked face —
//    statically decidable, INCLUDING FOR VIRTUAL DISPATCH (a `this`-receiver
//    dispatching call reaches the dynamic target's unchecked face). Calls on
//    *other* instances of the same class check normally. Invariant evaluation
//    itself runs as interior code, so a clause may call a `pure` public method
//    of the same instance WITHOUT RECURSION."
//
// #4597 implemented only the statically-bound arm. A `this`-receiver call to a
// `virtual` member loads its callee out of the object's vtable slot, which
// holds the CHECKED face and has no unchecked sibling — so both of #4597's
// symptoms survived verbatim for a `virtual` clause callee. Measured with
// `bezelc --checked-contracts` on the pre-fix compiler (this file's own
// FAIL-WITHOUT run, and identically on `origin/main`):
//
//   * `invariant(Ok());` over a `public virtual pure method Ok()` — unbounded
//     self-recursion in `bz$V.C$Ok`, one frame per level until the stack died:
//     SIGSEGV, exit 139, where §22.16 requires ContractViolationException.
//   * a public method that drives its instance through a state its own
//     invariant rejects and calls a `virtual` method on `this` while it is
//     there — 88 (a faulted invariant) where §22.16's "an algorithm may pass
//     through broken intermediate states" requires 3.
//
// ORACLE DISCIPLINE. The pre-fix behaviour of rows 1-3 is a CRASH, so every
// row asserts that the program RAN TO COMPLETION *and* returned the §22.16
// verdict — `runChecked` collapses "did not terminate normally" to the
// sentinel -999, so a row can never pass by crashing.
// `failedAtCompileTimeWithRealLocation` is deliberately NOT used anywhere
// here: it is vacuous against a crash, whose own backstop diagnostic carries
// a real location.
//
// Every row runs the real `bezelc` AOT path (fork-isolated
// Driver::compileFile + clang link + native exec).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

// §22.16 checked-contracts MODE — exactly `bezelc --checked-contracts`.
// -999 means the program did NOT terminate normally (a compile rejection, a
// compiler crash, a runtime signal, a timeout, or an ASan report) — the
// sentinel that keeps a crashing program from ever satisfying an EXPECT_EQ.
int runChecked(const std::string& program) {
    auto r = bezel::test::runAotCheckedContracts(program);
    return r.ran ? r.exitCode : -999;
}

// §22.16's unchecked (release) mode — the production default, in which
// `invariant` clauses are not evaluated at all (#4386).
int runRelease(const std::string& program) {
    auto r = bezel::test::runAot(program, bezel::codegen::CodegenOptions{});
    return r.ran ? r.exitCode : -999;
}

// ★ Row 1 — the crash, the #4597 repro verbatim except for `virtual`.
// The construction-exit check evaluates `Ok()`; `Ok` is virtual, so the call
// went through the vtable slot to the CHECKED face, whose prologue evaluated
// the invariant again. Pre-fix 139; §22.16 requires 99.
const char* kVirtualClauseCallee =
    "module V; class C "
    "  public k: int := 1; "
    "  invariant(Ok()); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public virtual pure method Ok(): bool return k > 0; end Ok; "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    try "
    "      using var c := new C(0 - 5); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end C;";

// ★ Row 2 — the clause reaches a virtual public method TRANSITIVELY. This is
// the row that decides the SHAPE of the fix: a remedy local to the clause's
// own lowering terminates row 1 and NOT this one, because `Helper()` is
// emitted inside `Ok`'s body where no such flag is set. Pre-fix 139.
const char* kVirtualClauseCalleeTransitive =
    "module V; class C "
    "  public k: int := 1; "
    "  invariant(Ok()); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public virtual pure method Ok(): bool return Helper(); end Ok; "
    "  public virtual pure method Helper(): bool return k > 0; end Helper; "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    try "
    "      using var c := new C(0 - 5); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end C;";

// ★ Row 3 — the clause is SATISFIED. The recursion is not a property of a
// violated predicate (the construction-exit check runs either way), so a
// CONFORMING program crashed too. Pre-fix 139; §22.16 requires `c.k` == 7.
const char* kVirtualClauseCalleeSatisfied =
    "module V; class C "
    "  public k: int := 1; "
    "  invariant(Ok()); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public virtual pure method Ok(): bool return k > 0; end Ok; "
    "  public static method Main(): int "
    "    using var c := new C(7); "
    "    return c.k; "
    "  end Main; "
    "end C;";

// ★ Row 4 — the non-crashing half, the `virtual` twin of row 4 of
// invariant_this_receiver_unchecked_face_4597_test.cpp (which passes).
// `BreakThenSelf` drives the instance through a state its own invariant
// rejects, calls `Read()` on `this` while it is there, and restores the
// invariant before returning. Pre-fix the `this`-receiver VIRTUAL call hit
// `Read`'s checked face and faulted (88); §22.16 requires 3.
const char* kThisReceiverVirtualCallMidUpdate =
    "module M; class C "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public virtual method Read(): int return k; end Read; "
    "  public method BreakThenSelf(): int "
    "    k := 0 - 3; "
    "    var t := Read(); "
    "    k := 5; "
    "    return 0 - t; "
    "  end BreakThenSelf; "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(2); "
    "    try "
    "      r := c.BreakThenSelf(); "
    "    catch ContractViolationException => r := 88; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end C;";

// ★ Row 5 — the DYNAMIC target is a derived override. The `this`-receiver
// call is written in `B`'s body and the receiver is a `D`, so §22.16's "a
// `this`-receiver dispatching call reaches the DYNAMIC target's unchecked
// face" is what this row pins: binding to `B::Read`'s face would run the
// wrong body, and binding to `D::Read`'s CHECKED face faults (88, pre-fix).
// §22.16 requires 3.
const char* kInheritedOverrideThisReceiverVirtualCall =
    "module M; class B "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public virtual method Read(): int return k; end Read; "
    "  public method BreakThenSelf(): int "
    "    k := 0 - 3; "
    "    var t := Read(); "
    "    k := 5; "
    "    return 0 - t; "
    "  end BreakThenSelf; "
    "end B; "
    "class D: B "
    "  public constructor(v: int) base(v); end constructor; "
    "  public override method Read(): int return k + 0; end Read; "
    "end D; "
    "class MainX "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new D(2); "
    "    try "
    "      r := c.BreakThenSelf(); "
    "    catch ContractViolationException => r := 88; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainX;";

// Control — §22.16 "Calls on *other* instances of the same class check
// normally", for a VIRTUAL callee. `a.Poke(b)` calls `b.Peek()` through a
// PARAMETER, not `this`, so it binds to the checked face and sees `b`'s
// broken invariant. 88 on BOTH sides of the fix: without this row the fix
// could bind every virtual call to the unchecked face and silently lose all
// invariant checking, and nothing here would notice.
const char* kOtherInstanceVirtualStillChecked =
    "module M; class C "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public virtual method Peek(): int return k; end Peek; "
    "  public method Poke(o: C): int return o.Peek(); end Poke; "
    "  public method Break(): int k := 0 - 9; return 1; end Break; "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var a := new C(3); "
    "    using var b := new C(4); "
    "    try "
    "      var ignored := b.Break(); "
    "      r := a.Poke(b); "
    "    catch ContractViolationException => r := 88; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end C;";

}  // namespace

// ── The crash rows ────────────────────────────────────────────────────────

BEZEL_TEST(invariant_virtual_this_receiver_unchecked_face_4613,
           clause_calls_virtual_pure_method_raises_not_recurses_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(kVirtualClauseCallee));
}

BEZEL_TEST(invariant_virtual_this_receiver_unchecked_face_4613,
           clause_reaches_virtual_method_transitively_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(kVirtualClauseCalleeTransitive));
}

BEZEL_TEST(invariant_virtual_this_receiver_unchecked_face_4613,
           satisfied_clause_calls_virtual_pure_method_spec_22_16) {
    BEZEL_EXPECT_EQ(7, runChecked(kVirtualClauseCalleeSatisfied));
}

// ── The over-rejection rows ───────────────────────────────────────────────

BEZEL_TEST(invariant_virtual_this_receiver_unchecked_face_4613,
           this_receiver_virtual_call_mid_update_spec_22_16) {
    BEZEL_EXPECT_EQ(3, runChecked(kThisReceiverVirtualCallMidUpdate));
}

BEZEL_TEST(invariant_virtual_this_receiver_unchecked_face_4613,
           dynamic_override_this_receiver_virtual_call_mid_update_spec_22_16) {
    BEZEL_EXPECT_EQ(3, runChecked(kInheritedOverrideThisReceiverVirtualCall));
}

// ── Controls (identical on both sides of the fix) ─────────────────────────

BEZEL_TEST(invariant_virtual_this_receiver_unchecked_face_4613,
           other_instance_virtual_call_still_checked_spec_22_16) {
    BEZEL_EXPECT_EQ(88, runChecked(kOtherInstanceVirtualStillChecked));
}

// §22.16 release default (#4386): no invariant is evaluated at all, so the
// violating construction completes and `Main` returns 42. Without this row a
// fix that emitted the §22.16 instrumentation unconditionally would pass.
BEZEL_TEST(invariant_virtual_this_receiver_unchecked_face_4613,
           virtual_clause_callee_unchecked_mode_control_spec_22_16) {
    BEZEL_EXPECT_EQ(42, runRelease(kVirtualClauseCallee));
}

BEZEL_TEST(invariant_virtual_this_receiver_unchecked_face_4613,
           this_receiver_virtual_call_unchecked_mode_control_spec_22_16) {
    BEZEL_EXPECT_EQ(3, runRelease(kThisReceiverVirtualCallMidUpdate));
}
