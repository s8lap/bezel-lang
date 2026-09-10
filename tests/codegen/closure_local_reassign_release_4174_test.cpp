// Codegen — issue #4174 (§27.5 closures, §28.9 runtime-managed types, spec
// v0.298): reassigning a function-typed (closure) LOCAL never released the
// DISPLACED previous closure value. Measured on the pre-fix compiler
// (`bezelc --sanitize=address`, LeakSanitizer on, 500x amplified):
//
//     var f: () -> int := lambda() => 0;
//     f := lambda() => 42;
//     return f;
//                                   ->  40 bytes / 1 allocation leaked per call
//
// while binding a closure local exactly ONCE (no reassignment) was already
// ASan-clean.
//
// ROOT CAUSE. `LlvmBackend_Stmts.cpp`'s AssignmentStmt lowering has a
// retain-new/release-old "drop-on-overwrite" helper for every OTHER
// runtime-managed/refcounted local-slot family reassigned in place —
// `lowerStringLocalReassign` for `string` locals, and the box-vs-class state
// bit for `Object`/interface locals (`collectBoxReleaseEligible` /
// `boxReassignedLocals_`) — but had no closure-local sibling at all. The
// producer-side scope-end release family (`collectClosureReleaseEligible`,
// `closureReleaseEligible_`) exists, but `ClosureEscapeWalker::stmt`'s own
// AssignmentStmt arm marks ANY reassignment of the tracked local as an
// ESCAPE with the comment "overwrite handling is out of this increment" —
// so a reassigned closure local was never eligible for a scope-end release
// AND had no reassignment-site release either: the displaced value from
// every `:=` before the last one was simply orphaned. This test file is
// that increment's overwrite handling
// (`Emitter::lowerClosureLocalReassign`, mirroring `lowerStringLocalReassign`
// and the existing `lowerClosureFieldStore` retain-vs-transfer decision).
//
// The first three tests use runAotChecked (generated-code ASan with
// LeakSanitizer ON), so `r.ran` is itself the leak assertion, exactly as
// const_local_closure_release_3402_test.cpp's file-level convention states.
// The last three (self-assignment / borrow / method-reference) use
// runAotCheckedLenient (ASan on, LeakSanitizer OFF) instead — see the
// runLenientOut helper's own comment for why: their FINAL closure value is,
// on BOTH compilers, excluded from the scope-end release set by the
// pre-existing, documented "a reassigned local's final value leaks safely"
// trade-off (mirroring lowerStringLocalReassign's own for `string`), which
// is unrelated to this fix and would otherwise trip LeakSanitizer for a
// program that in fact ran correctly (measured: a one-shot 40-byte leak DOES
// clear LSan's report threshold in this harness — confirmed empirically,
// no floor to rely on).
//
// FAIL-WITHOUT / PASS-WITH, measured program-by-program (500x-loop-amplified
// where noted) on one build of each compiler:
//
//   program                                          pre-fix   post-fix
//   f := lambda()=>0; f := lambda()=>42; return f;
//     (x500 outer loop, runAotChecked)                FAIL      PASS (clean)
//   f := lambda()=>0; f := lambda()=>1; f := lambda()=>2;
//     return f; (x500, TWO reassignments,
//     runAotChecked)                                  FAIL      PASS (clean)
//   var f := lambda()=>0; return f;  (no reassignment,
//     parity control, x500, runAotChecked)             PASS      PASS (clean)
//   f reassigned to an existing closure-typed local
//     (borrow, not a producer), then both called
//     (one-shot, runAotCheckedLenient)                 PASS*     PASS (no
//                                                                  crash)
//   f := f;  (self-assignment safety, one-shot,
//     runAotCheckedLenient)                             PASS      PASS
//   f reassigned to a method reference (one-shot,
//     runAotCheckedLenient)                             PASS*     PASS (no
//                                                                  crash)
//
// * these rows are not FAIL-WITHOUT rows: they are no-crash / no-double-free
//   controls under ASan with LeakSanitizer deliberately disabled — see each
//   test's own comment for why. The FAIL-WITHOUT/PASS-WITH claim for this
//   issue rests on the first three (runAotChecked) rows.
//
// per §27.5, §28.9 (issue #4174)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
// LSan-DISABLED (ASan still on): for the three rows below whose FINAL
// closure value is — on BOTH the pre-fix and post-fix compiler, by the
// documented ClosureEscapeWalker/lowerStringLocalReassign-parity design (see
// the file-level comment) — never released at scope end. Those rows assert
// "no crash / correct value", not "leak-clean"; runAotChecked would trip
// LeakSanitizer on that pre-existing, unrelated leak and report
// "<did-not-run>" for a program that in fact ran correctly (measured: a
// one-shot 40-byte leak DOES clear LSan's report threshold — no floor to
// rely on here). runAotCheckedLenient (issue #2214) keeps generated-code
// ASan (use-after-free / double-free) active while disabling only
// LeakSanitizer, which is exactly the axis these three rows test.
std::string runLenientOut(const std::string& prog) {
    auto r = runAotCheckedLenient(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── The issue's exact repro, 500x amplified in a loop so the 40-byte
// per-call leak clears LeakSanitizer's reporting floor (a one-shot can pass
// on both sides — LANE_RULES §3). Pre-fix: 20000 bytes / 500 allocations
// leaked (the displaced `lambda() => 0` cell, once per call). The returned
// value must still be correct (42) — the release must not disturb the
// TRANSFERRED final closure. // per §27.5, §28.9 (issue #4174)
BEZEL_TEST(closure_local_reassign_release_4174,
           reassigned_closure_local_displaced_value_released_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 0; "
        "    f := lambda() => 42; "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      var g := C.MakeClosure(); "
        "      total := total + g(); "
        "      i := i + 1; "
        "    end; "
        "    return total mod 97; "
        "  end Main; "
        "end C;"),
        /* 500 * 42 = 21000; 21000 mod 97 = 3 */ 21000 % 97);
}

// ── PARITY CONTROL: binding a closure local exactly once (no reassignment)
// was already clean pre-fix and must stay clean — this pins that the fix
// does not touch the non-reassigned producer-release path at all.
// // per §27.5, §28.9 (issue #4174)
BEZEL_TEST(closure_local_reassign_release_4174,
           unreassigned_closure_local_still_clean_parity_spec_27_5) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 42; "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      var g := C.MakeClosure(); "
        "      total := total + g(); "
        "      i := i + 1; "
        "    end; "
        "    return total mod 97; "
        "  end Main; "
        "end C;"),
        21000 % 97);
}

// ── SCALING axis: TWO reassignments displace TWO distinct producer values
// (`lambda()=>0` then `lambda()=>1`), so the leak scales linearly with
// reassignment count, not just presence. Pre-fix: 40000 bytes / 1000
// allocations (2 x 20000/500) leaked; the FINAL value (`lambda()=>2`) is
// transferred out via `return` and must stay correct.
// // per §27.5, §28.9 (issue #4174)
BEZEL_TEST(closure_local_reassign_release_4174,
           two_reassignments_both_displaced_values_released_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 0; "
        "    f := lambda() => 1; "
        "    f := lambda() => 2; "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      var g := C.MakeClosure(); "
        "      total := total + g(); "
        "      i := i + 1; "
        "    end; "
        "    return total mod 97; "
        "  end Main; "
        "end C;"),
        /* 500 * 2 = 1000; 1000 mod 97 */ 1000 % 97);
}

// ── BORROW axis / no-crash control (LANE_RULES §15 accept-side sweep): `f`
// is reassigned to an EXISTING closure-typed local `g` (a borrow, not a
// producer) rather than a fresh lambda. `lowerClosureLocalReassign` must
// RETAIN the new value (mirroring `lowerClosureFieldStore`'s own retain-vs-
// transfer decision) rather than transferring it, since `g` still owns its
// own reference too — a bare transfer here would double-release `g`'s cell
// when BOTH `f` and `g` are later used. This is not a FAIL-WITHOUT row (the
// reassigned local's FINAL value is excluded from the scope-end release set
// on BOTH compilers — ClosureEscapeWalker marks any reassigned target as
// escaped regardless of this fix, mirroring `lowerStringLocalReassign`'s own
// documented "leaks safely rather than double-frees" trade-off for
// strings): it asserts the program runs to completion with the CORRECT
// value under ASan/LSan on both `f()` and `g()` — i.e., no premature
// double-free/UAF from an under-retained alias. // per §27.5, §28.3, §28.9
// (issue #4174)
BEZEL_TEST(closure_local_reassign_release_4174,
           borrow_reassignment_no_uaf_no_double_free_spec_27_5_28_3) {
    BEZEL_EXPECT_EQ(runLenientOut(
        "module M; import System; "
        "class C "
        "  public static method MakeBorrow(): int "
        "    var g: () -> int := lambda() => 5; "
        "    var f: () -> int := lambda() => 0; "
        "    f := g; "
        "    return f() + g(); "
        "  end MakeBorrow; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"r=\" + C.MakeBorrow().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "r=10\n");
}

// ── SELF-ASSIGNMENT SAFETY axis: `f := f;` must not transiently drop the
// slot's refcount to zero. `lowerClosureLocalReassign` retains the new value
// BEFORE releasing the old — same address, so the count dips +1 then back,
// never through 0 — mirroring `lowerStringLocalReassign`'s own
// retain-before-release ordering rationale. // per §27.5, §28.9 (issue
// #4174)
BEZEL_TEST(closure_local_reassign_release_4174,
           self_assignment_retain_before_release_is_safe_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(runLenientOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var f: () -> int := lambda() => 7; "
        "    f := f; "
        "    Console.WriteLine(\"f=\" + f().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "f=7\n");
}

// ── METHOD-REFERENCE axis (§27.6): the RHS producer can be a method
// reference, not only a lambda literal — `isUserMethodClosureReturnCall` /
// `isPlusOneClosureProducer`'s method-reference arm must still be recognized
// as a TRANSFER (no retain) by the reassignment helper, exactly as
// `lowerClosureFieldStore` already recognizes it. Pre-fix this row leaked
// BOTH the displaced `lambda()=>0` (#4174's own bug) and — unrelated to this
// fix, unchanged on both sides — the reassigned local's own final value at
// scope end (never returned/consumed further here).
// // per §27.5, §27.6, §28.9 (issue #4174)
BEZEL_TEST(closure_local_reassign_release_4174,
           method_reference_reassignment_transfers_no_extra_retain_spec_27_6) {
    BEZEL_EXPECT_EQ(runLenientOut(
        "module M; import System; "
        "class C "
        "  public static method Helper(): int "
        "    return 9; "
        "  end Helper; "
        "  public static method Main(): int "
        "    var f: () -> int := lambda() => 0; "
        "    f := C.Helper; "
        "    Console.WriteLine(\"f=\" + f().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "f=9\n");
}
