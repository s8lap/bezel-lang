// Spec-derived runtime tests for §22.16's `this`-reentrancy exemption — the
// UNCHECKED FACE (v0.306, issue #4597).
//
// §22.16 "The visibility boundary and the `this`-reentrancy exemption":
//
//   "A call whose receiver is `this` — explicit or implicit — from inside any
//    instance member of the class or a subclass enters the member's unchecked
//    face: no invariant evaluation at that call's entry or exit. Every
//    instrumented public member compiles to a checked face (entry checks →
//    unchecked face → exit checks) and an unchecked face (the body); call
//    sites with syntactic receiver `this` bind to the unchecked face —
//    statically decidable […]. Calls on *other* instances of the same class
//    check normally. Invariant evaluation itself runs as interior code, so a
//    clause may call a `pure` public method of the same instance WITHOUT
//    RECURSION."
//
// That last sentence was FALSE in the implementation. `emitInvariantCheck`
// (src/codegen/LlvmBackend_Setup.cpp) guarded re-entry with
// `insideInvariantCheck_`, a COMPILE-TIME flag held only while a predicate is
// being lowered. It never reached the emitted call: the clause emitted a call
// to the callee's ONE face, whose prologue re-evaluated the invariant, which
// called it again. Measured on origin/main @ cafdfc52 with
// `bezelc --checked-contracts`, `invariant(Ok());` over a `public pure method
// Ok(): bool` — unbounded self-recursion in `bz$M.C$Ok` (gdb: every frame the
// same return address) until the stack died: SIGSEGV, exit 139, where §22.16
// requires ContractViolationException.
//
// The same missing lowering had a second, non-crashing face: a public method
// calling `this.Other()` while its own state is legitimately mid-update hit
// `Other`'s checked face and faulted, against §22.16's "it may be freely
// false while the instance is operating on itself".
//
// ORACLE DISCIPLINE. The pre-fix behaviour of rows 1-3 is a CRASH, so every
// row asserts that the program RAN TO COMPLETION *and* returned the §22.16
// verdict — `runChecked` collapses "did not terminate normally" to the
// sentinel -999 (JitResult::ran is false for a runtime signal), so a row can
// never pass by crashing. `failedAtCompileTimeWithRealLocation` is deliberately
// NOT used anywhere here: it is vacuous against a crash, whose own backstop
// diagnostic carries a real location.
//
// Every row runs the real `bezelc` AOT path (fork-isolated
// Driver::compileFile + clang link + native exec).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

// §22.16 checked-contracts MODE — exactly `bezelc --checked-contracts`, the
// mode in which the invariant instrumentation set is emitted at all.
// -999 means the program did NOT terminate normally (a compile rejection, a
// compiler crash, a runtime signal, a timeout, or an ASan report) — the
// sentinel that keeps a crashing program from ever satisfying an EXPECT_EQ.
int runChecked(const std::string& program) {
    auto r = bezel::test::runAotCheckedContracts(program);
    return r.ran ? r.exitCode : -999;
}

// §22.16's unchecked (release) mode — the production default, in which
// `ensures` and `invariant` clauses are not evaluated at all (#4386). Every
// row that has a checked twin above also runs here, so a fix that emitted the
// §22.16 instrumentation unconditionally would be caught.
int runRelease(const std::string& program) {
    auto r = bezel::test::runAot(program, bezel::codegen::CodegenOptions{});
    return r.ran ? r.exitCode : -999;
}

// ★ Row 1's program, shared by the checked and release rows so the ONLY
// difference between them is the mode bit.
const char* kClauseCallsPureMethod =
    "module M; class C "
    "  public k: int := 1; "
    "  invariant(Ok()); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public pure method Ok(): bool return k > 0; end Ok; "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    try "
    "      using var c := new C(-5); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end C;";

// ★ Row 2's program: the clause reaches the public method TRANSITIVELY.
// This is the row that decides the SHAPE of the fix. A remedy local to the
// clause — "while lowering an invariant predicate, route this call past the
// check" — terminates row 1 and NOT this one: `Helper()` is emitted inside
// `Ok`'s body, where no such flag is set, so `Helper`'s checked face
// re-evaluates the invariant, which calls `Ok` again. Measured: 139 on the
// pre-fix compiler, identical to row 1.
const char* kClauseReachesPureMethodTransitively =
    "module M; class C "
    "  public k: int := 1; "
    "  invariant(Ok()); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public pure method Ok(): bool return Helper(); end Ok; "
    "  public pure method Helper(): bool return k > 0; end Helper; "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    try "
    "      using var c := new C(-5); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end C;";

// ★ Row 3's program: the clause is SATISFIED. The recursion is not a property
// of a violated predicate — the construction-exit check runs either way — so a
// conforming program crashed too. Pre-fix 139; §22.16 requires `c.k` == 7.
const char* kSatisfiedClauseCallsPureMethod =
    "module M; class C "
    "  public k: int := 1; "
    "  invariant(Ok()); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public pure method Ok(): bool return k > 0; end Ok; "
    "  public static method Main(): int "
    "    using var c := new C(7); "
    "    return c.k; "
    "  end Main; "
    "end C;";

// ★ Row 4's program: the non-crashing half of the same missing lowering.
// `BreakThenSelf` drives the instance through a state its own invariant
// rejects, calls `Read()` on `this` while it is there, and restores the
// invariant before returning — §22.16's "an algorithm may pass through broken
// intermediate states". Pre-fix the `this`-receiver call hit `Read`'s checked
// face and faulted (88); §22.16 requires 3.
const char* kThisReceiverCallMidUpdate =
    "module M; class C "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public method Read(): int return k; end Read; "
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

// ★ Row 5's program: the §22.7 composition. A CONSTRUCTOR body calls a public
// method on `this` while the instance is still being assembled — §22.16's
// interior, and §22.16 "Construction composes with §22.7 exactly": the only
// construction-time instrumentation point is the most-derived constructor's
// normal completion, by which time `k` is 9. Pre-fix the call hit `Init`'s
// checked face over `k == 0` and faulted (88).
const char* kConstructorCallsPublicMethodOnThis =
    "module M; class C "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor() "
    "    k := 0; "
    "    Init(); "
    "  end constructor; "
    "  public method Init(): int "
    "    k := 9; "
    "    return k; "
    "  end Init; "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    try "
    "      using var c := new C(); "
    "      r := c.k; "
    "    catch ContractViolationException => r := 88; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end C;";

// Control: §22.16 "Calls on *other* instances of the same class check
// normally". `a.Poke(b)` calls `b.Peek()` through a PARAMETER, not `this`, so
// it binds to the checked face and sees `b`'s broken invariant.
const char* kOtherInstanceStillChecked =
    "module M; class C "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public method Peek(): int return k; end Peek; "
    "  public method Poke(o: C): int return o.Peek(); end Poke; "
    "  public method Break(): int k := 0 - 9; return 1; end Break; "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var a := new C(3); "
    "    using var b := new C(4); "
    "    try "
    "      var t := b.Break(); "
    "    catch ContractViolationException => r := r + 10; "
    "    end; "
    "    try "
    "      var u := a.Poke(b); "
    "    catch ContractViolationException => r := r + 7; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end C;";

// Control: the CHECKED face is still fully instrumented. `c.Break()` is an
// external call whose normal exit leaves the invariant false (+20, the §22.16
// public-method-EXIT point), and the following external `c.Read()` faults at
// ENTRY over the state it left (+3).
const char* kCheckedFaceStillInstrumented =
    "module M; class C "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public method Break(): int k := 0 - 4; return 1; end Break; "
    "  public method Read(): int return k; end Read; "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(2); "
    "    try "
    "      var b := c.Break(); "
    "      r := r + 2; "
    "    catch ContractViolationException => r := r + 20; "
    "    end; "
    "    try "
    "      var d := c.Read(); "
    "      r := r + 4; "
    "    catch ContractViolationException => r := r + 3; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end C;";

// Control: a class declaring NO invariant member. §22.16 instruments nothing
// for it, so no face is minted and a `this`-receiver call is the same single
// call it always was. Pins that the two-face machinery is inert for the
// overwhelmingly common class.
const char* kNoInvariantUnaffected =
    "module M; class C "
    "  public k: int := 1; "
    "  public method Read(): int return k; end Read; "
    "  public method Chain(): int "
    "    k := 11; "
    "    return Read(); "
    "  end Chain; "
    "  public static method Main(): int "
    "    using var c := new C(); "
    "    return c.Chain(); "
    "  end Main; "
    "end C;";

}  // namespace

// =========================================================================
// ★ Row 1 (§22.16, v0.306) — THE BUG. An invariant clause calling a `pure`
// public method of the same instance runs "without recursion": the clause
// binds to `Ok`'s unchecked face, the predicate evaluates once, and the
// violated invariant at construction exit raises ContractViolationException.
//
// FAIL-WITHOUT: 139 (SIGSEGV) pre-fix, so `runChecked` returns -999.
// =========================================================================
BEZEL_TEST(invariant_this_receiver_unchecked_face_4597,
           clause_calls_pure_public_method_without_recursion_spec_22_16) {
    BEZEL_EXPECT_EQ(runChecked(kClauseCallsPureMethod), 99);
}

// =========================================================================
// ★ Row 2 (§22.16, v0.306) — the TRANSITIVE shape. `invariant(Ok());` where
// `Ok` itself calls `Helper()` on `this`. Every `this`-receiver call in the
// chain binds to an unchecked face, so the cycle is closed wherever it starts.
//
// FAIL-WITHOUT: 139 pre-fix (-999 through the sentinel).
// =========================================================================
BEZEL_TEST(invariant_this_receiver_unchecked_face_4597,
           clause_reaches_public_method_transitively_spec_22_16) {
    BEZEL_EXPECT_EQ(runChecked(kClauseReachesPureMethodTransitively), 99);
}

// =========================================================================
// ★ Row 3 (§22.16, v0.306) — a CONFORMING program. The clause holds, so no
// exception is due at all; the construction-exit check must simply evaluate
// once and let the program run.
//
// FAIL-WITHOUT: 139 pre-fix — the crash was independent of the predicate's
// truth, which is why this row is not redundant with row 1.
// =========================================================================
BEZEL_TEST(invariant_this_receiver_unchecked_face_4597,
           satisfied_clause_calls_pure_public_method_spec_22_16) {
    BEZEL_EXPECT_EQ(runChecked(kSatisfiedClauseCallsPureMethod), 7);
}

// =========================================================================
// ★ Row 4 (§22.16, v0.306) — the over-rejection half. A `this`-receiver call
// made while the instance is legitimately mid-update enters the unchecked
// face and does not fault; the method's own exit check, over the restored
// state, passes.
//
// FAIL-WITHOUT: 88 pre-fix (the spurious ContractViolationException).
// =========================================================================
BEZEL_TEST(invariant_this_receiver_unchecked_face_4597,
           this_receiver_call_mid_update_is_admitted_spec_22_16) {
    BEZEL_EXPECT_EQ(runChecked(kThisReceiverCallMidUpdate), 3);
}

// =========================================================================
// ★ Row 5 (§22.16 / §22.7, v0.306) — a constructor body's `this`-receiver
// call is interior too. §22.16's construction-time instrumentation point is
// the most-derived constructor's NORMAL COMPLETION and nothing earlier, so a
// public method invoked from the ctor body must not evaluate the invariant
// over a half-assembled instance.
//
// FAIL-WITHOUT: 88 pre-fix (the spurious ContractViolationException at
// `Init`'s checked-face entry, over `k == 0`).
// =========================================================================
BEZEL_TEST(invariant_this_receiver_unchecked_face_4597,
           constructor_body_this_receiver_call_is_interior_spec_22_16) {
    BEZEL_EXPECT_EQ(runChecked(kConstructorCallsPublicMethodOnThis), 9);
}

// =========================================================================
// Row 6 (§22.16 control) — "Calls on *other* instances of the same class
// check normally". Green on BOTH sides of the fix: the exemption must not
// widen from `this` to "any receiver of the same class".
// =========================================================================
BEZEL_TEST(invariant_this_receiver_unchecked_face_4597,
           other_instance_receiver_still_checked_spec_22_16) {
    BEZEL_EXPECT_EQ(runChecked(kOtherInstanceStillChecked), 17);
}

// =========================================================================
// Row 7 (§22.16 control) — the checked face keeps BOTH its instrumentation
// points: the public-method exit check (+20) and the public-method entry
// check (+3). Green on both sides; this is the row a fix that suppressed the
// invariant everywhere instead of only on `this`-receiver calls would fail.
// =========================================================================
BEZEL_TEST(invariant_this_receiver_unchecked_face_4597,
           checked_face_entry_and_exit_still_fire_spec_22_16) {
    BEZEL_EXPECT_EQ(runChecked(kCheckedFaceStillInstrumented), 23);
}

// =========================================================================
// Row 8 (§22.16 control) — a class with no `invariant` member is untouched.
// Green on both sides.
// =========================================================================
BEZEL_TEST(invariant_this_receiver_unchecked_face_4597,
           class_without_invariant_unaffected_spec_22_16) {
    BEZEL_EXPECT_EQ(runChecked(kNoInvariantUnaffected), 11);
}

// =========================================================================
// Rows 9-13 (§22.16 release mode, #4386) — the SAME five programs in the
// unchecked (release) default, where "unproven `ensures` and `invariant`
// clauses are not evaluated". Nothing is instrumented, so nothing can recurse
// and nothing can fault: every program returns its uninstrumented value, and
// these rows are green on both sides of the fix.
//
// They are the guard against the opposite failure — a face split that leaked
// invariant evaluation into the mode that must not have it, or that emitted
// the §22.16 instrumentation unconditionally.
// =========================================================================
BEZEL_TEST(invariant_this_receiver_unchecked_face_4597,
           release_mode_clause_calls_pure_method_not_evaluated_spec_22_16) {
    BEZEL_EXPECT_EQ(runRelease(kClauseCallsPureMethod), 42);
}

BEZEL_TEST(invariant_this_receiver_unchecked_face_4597,
           release_mode_transitive_clause_not_evaluated_spec_22_16) {
    BEZEL_EXPECT_EQ(runRelease(kClauseReachesPureMethodTransitively), 42);
}

BEZEL_TEST(invariant_this_receiver_unchecked_face_4597,
           release_mode_satisfied_clause_program_runs_spec_22_16) {
    BEZEL_EXPECT_EQ(runRelease(kSatisfiedClauseCallsPureMethod), 7);
}

BEZEL_TEST(invariant_this_receiver_unchecked_face_4597,
           release_mode_this_receiver_call_mid_update_spec_22_16) {
    BEZEL_EXPECT_EQ(runRelease(kThisReceiverCallMidUpdate), 3);
}

BEZEL_TEST(invariant_this_receiver_unchecked_face_4597,
           release_mode_constructor_this_receiver_call_spec_22_16) {
    BEZEL_EXPECT_EQ(runRelease(kConstructorCallsPublicMethodOnThis), 9);
}

// =========================================================================
// Row 14 (§22.16 release-mode control) — the checked-face program in release
// mode takes the NO-fault path through both `try` arms (2 + 4), the exact
// complement of row 6's 20 + 3. Both observables stay under 256, so neither
// is confused by `WEXITSTATUS` truncation.
// =========================================================================
BEZEL_TEST(invariant_this_receiver_unchecked_face_4597,
           release_mode_no_instrumentation_at_all_spec_22_16) {
    BEZEL_EXPECT_EQ(runRelease(kCheckedFaceStillInstrumented), 6);
}
