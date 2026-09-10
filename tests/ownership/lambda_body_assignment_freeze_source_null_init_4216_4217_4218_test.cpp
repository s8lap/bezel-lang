// Spec-derived regression tests for issues #4216, #4217 and #4218 — the
// three pieces of `handleAssignment`'s bare-identifier target-state
// transition that fix #4187/#3972's lambda-body port deliberately left out,
// on the (WRONG, review-falsified) belief that none was reachable from that
// arm. `MethodOwnership::applyUsingVarBorrowPinInLambdaStmt`'s AssignmentStmt
// arm now runs the same `checkConsumeFreeze` call, the same §28.9
// borrow-source recompute, and the same null-init `becomesOwnedViaReassign`
// flag as the real per-method `handleAssignment` (Ownership.cpp ~6650 on),
// each gated exactly as it is there.
//
// #4216 — §28.7 (v0.298) "Overwrite while borrowed": "Reassigning a
// plain-`var` owned local while a live local borrow of its current value
// exists is rejected by the consume-site freeze (K5.2; §28.3)." Before this
// fix, reassigning a lambda-body owned local while a `var y := x;`-style
// borrow of it was still live compiled with ZERO ownership errors and read
// freed memory (measured: AddressSanitizer heap-use-after-free, freed inside
// the lambda's own generated code). checkConsumeFreeze already runs from
// THIS exact arm's context: this arm's own `recordMovesIn` call (a few lines
// above the new block) routes `move x` through `markMovedByName`, which
// calls `checkConsumeFreeze` (Ownership_Checks.cpp:9620) — so the missing
// call was the ONE remaining site in the family without it, not a function
// nothing here could reach.
//
// #4217 — §28.9 / §9.7: a borrow holder's `source` must track the CURRENT
// thing it borrows, not whatever it borrowed before a rebind. Before this
// fix, `y := x;` (rebinding an existing borrow holder `y` to borrow `x`)
// never updated `y.source`, so a later `move x` found no live holder to
// freeze against (y's stale source still named the OLD borrowed local) and
// silently succeeded — then reading `y` afterward read freed memory
// (measured: AddressSanitizer heap-use-after-free, freed inside the
// generated code of the `owned`-parameter sink the move fed).
//
// #4218 — §28.7 / §28.3 (#1625): a null-initialized plain local
// (`var x: T? := null;`) that first adopts an Owned value via reassignment
// needs the `becomesOwnedViaReassign` flag so codegen's
// `unwindOwnedLocalClass` classifies it as an owned class local for BOTH
// drop-on-overwrite (a later reassignment's displaced value) and disposal
// generally. Before this fix, inside a lambda body this flag was never set,
// so even an EXPLICIT `.Dispose()` call on the local's final value did not
// free it (measured: LeakSanitizer, 32 bytes / 2 allocations — both the
// value displaced by a second reassignment AND the final explicitly-disposed
// value leaked).
//
// All three are PRE-EXISTING (unrelated to #4187/#3972's own code — the
// three method-body control rows below establish that the identical shape
// was ALREADY handled correctly at method scope on this same tree) and
// reachable only because #4187/#3972 made the lambda-body assignment arm do
// ANY target-state transition at all; before that fix these three shapes
// were unreachable through this arm (nothing there ever classified a target
// as Owned in the first place).
//
// per §28.3, §28.7, §28.9, §9.7, v0.298, #4216, #4217, #4218

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "support/Diagnostic.hpp"
#include "support/ownership_support.hpp"   // parseAndCheckOwnership, hasErrorMessage
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

const char* kFreezeWording =
    "cannot consume `x` here: the borrow held by `y` is still live — its "
    "later use would read freed memory (consume-site freeze, "
    "§28.3/§28.7/§30.14 v0.180)";

// Res without a static counter — used by the #4216/#4217 rows, which do not
// need leak counting (they are rejection/UAF rows, not disposal-count rows).
const char* kRes =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; "
    "end Res; "
    "class Sink "
    "  public static method Take(owned r: Res): int return r.tag; end Take; "
    "end Sink; ";

// Destructor-counting Res for the #4218 leak rows (matches the family's
// established idiom).
const char* kResCounted =
    "module M; "
    "class Res "
    "  public static live: int := 0; "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; Res.live := Res.live + 1; end constructor; "
    "  destructor Res.live := Res.live - 1; end destructor; "
    "end Res; ";

}  // namespace

// ===========================================================================
// #4216 — checkConsumeFreeze must run from the lambda-body assignment arm.
// ===========================================================================

// Row 1 (diagnostic-only): `var y := x;` borrows x; reassigning x while y is
// still live (used after) must reject with the exact §28.3/§28.7/§30.14
// wording.
BEZEL_TEST(lambda_body_assignment_freeze_source_null_init_4216_4217_4218,
           lambda_reassign_while_borrowed_rejected_4216) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var y := x; "
        "      x := new Res(9); "
        "      var t := y.tag; "
        "      x.Dispose(); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, std::string(kFreezeWording)));
}

// Row 2 (real execution): pairs Row 1 with message-text on the COMPILE-TIME
// path (lane rule: a bare compile-time helper is not enough when the
// pre-fix behaviour is a crash). FAIL-WITHOUT this fix: the program compiled
// rc=0 with zero ownership diagnostics and heap-use-after-freed under
// `--sanitize=address` (measured on this branch's parent). PASS-WITH:
// rejected at compile time with the freeze wording, never reaching codegen.
BEZEL_TEST(lambda_body_assignment_freeze_source_null_init_4216_4217_4218,
           lambda_reassign_while_borrowed_crash_fixed_4216) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var y := x; "
        "      x := new Res(9); "
        "      var t := y.tag; "
        "      x.Dispose(); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreezeWording));
}

// Row 3 — parity control: the byte-identical method-body (non-lambda)
// spelling produces the IDENTICAL wording (this was already correctly
// rejected before #4187/#3972/#4216 — pins that the lambda-body port is not
// a second, drifting spelling of the same rule, lane-rules §6).
BEZEL_TEST(lambda_body_assignment_freeze_source_null_init_4216_4217_4218,
           method_body_reassign_while_borrowed_matches_wording_control_4216) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    x := new Res(9); "
        "    var t := y.tag; "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, std::string(kFreezeWording)));
}

// Row 4 (accept-side control, §15, real execution): `y`'s last use is
// BEFORE the reassignment — the freeze must NOT false-reject a dead borrow.
// Value-checked: returns 5 (Res(5).tag, read before the reassignment).
BEZEL_TEST(lambda_body_assignment_freeze_source_null_init_4216_4217_4218,
           lambda_reassign_after_last_borrow_use_accepted_control_4216) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var y := x; "
        "      var u := y.tag; "
        "      x := new Res(9); "
        "      x.Dispose(); "
        "      return u; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// #4217 — the §28.9 borrow-source recompute on a rebound holder.
// ===========================================================================

// Row 5 (diagnostic-only): `y` initially borrows `z`; `y := x;` rebinds it
// to borrow `x`. A later `move x` while `y` (now sourced on `x`) is still
// live must reject — same wording, now anchored at the `move` site.
BEZEL_TEST(lambda_body_assignment_freeze_source_null_init_4216_4217_4218,
           lambda_move_after_rebind_rejected_4217) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var z := new Res(1); "
        "      var y := z; "
        "      y := x; "
        "      var t := Sink.Take(move x); "
        "      var v := y.tag; "
        "      z.Dispose(); "
        "      return v + t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, std::string(kFreezeWording)));
}

// Row 6 (real execution): FAIL-WITHOUT this fix: rc=0, zero ownership
// diagnostics, heap-use-after-freed under `--sanitize=address` (freed
// inside the `owned`-parameter sink `Sink.Take`, measured on this branch's
// parent). PASS-WITH: rejected at compile time, freeze wording.
BEZEL_TEST(lambda_body_assignment_freeze_source_null_init_4216_4217_4218,
           lambda_move_after_rebind_crash_fixed_4217) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var z := new Res(1); "
        "      var y := z; "
        "      y := x; "
        "      var t := Sink.Take(move x); "
        "      var v := y.tag; "
        "      z.Dispose(); "
        "      return v + t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreezeWording));
}

// Row 7 — parity control: the byte-identical method-body spelling produces
// the IDENTICAL wording (already correct at method scope before this fix).
BEZEL_TEST(lambda_body_assignment_freeze_source_null_init_4216_4217_4218,
           method_body_move_after_rebind_matches_wording_control_4217) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var z := new Res(1); "
        "    var y := z; "
        "    y := x; "
        "    var t := Sink.Take(move x); "
        "    var v := y.tag; "
        "    z.Dispose(); "
        "    return v + t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, std::string(kFreezeWording)));
}

// Row 8 (accept-side control, §15, real execution): `y` is rebound to `x`
// but never read again afterward — the recomputed source must not
// false-reject a holder with no live use. Value-checked: returns 5
// (Sink.Take's return, Res(5).tag).
BEZEL_TEST(lambda_body_assignment_freeze_source_null_init_4216_4217_4218,
           lambda_rebind_then_move_no_later_use_accepted_control_4217) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var z := new Res(1); "
        "      var y := z; "
        "      y := x; "
        "      var t := Sink.Take(move x); "
        "      z.Dispose(); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// #4218 — the null-init becomesOwnedViaReassign flag.
// ===========================================================================

// Row 9 (real execution, LOOP-AMPLIFIED 500x per
// [[lsan-one-shot-leak-tests-pass-vacuously]]): a null-initialized local
// (`var x: Res? := null;`) adopts Owned via reassignment TWICE — the first
// value is displaced by the second reassignment (drop-on-overwrite), the
// second is explicitly `.Dispose()`d. FAIL-WITHOUT this fix (measured
// single-shot on this branch's parent): 32 bytes / 2 allocations leaked —
// BOTH values, including the one explicitly `.Dispose()`d, since neither is
// ever classified as an owned class local without the flag. PASS-WITH:
// `Res.live == 0` — exact-once disposal of all 1000 allocations (500
// displaced + 500 explicitly disposed). Uses a fixed literal, not the outer
// loop counter, inside the lambda — see issue 4210 (an unrelated
// capture-triggered leak this sidesteps, filed during the #4187/#3972 work).
BEZEL_TEST(lambda_body_assignment_freeze_source_null_init_4216_4217_4218,
           lambda_null_init_first_adopt_reassign_disposes_once_amplified_4218) {
    std::string src =
        std::string(kResCounted) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var f := lambda(): int => do "
        "        var x: Res? := null; "
        "        x := new Res(5); "
        "        x := new Res(9); "
        "        x.Dispose(); "
        "        return 1; "
        "      end; "
        "      _ := f(); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 10 — parity control (real execution, single-shot is sufficient: this
// shape was ALREADY correct at method scope before this batch, on the
// #1625 mechanism this fix ports unchanged; establishes the pre-existing
// baseline the amplified lambda row above is being brought up to).
BEZEL_TEST(lambda_body_assignment_freeze_source_null_init_4216_4217_4218,
           method_body_null_init_first_adopt_reassign_disposes_once_control_4218) {
    std::string src =
        std::string(kResCounted) +
        "class C "
        "  public static method Main(): int "
        "    var x: Res? := null; "
        "    x := new Res(5); "
        "    x := new Res(9); "
        "    x.Dispose(); "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
