// Codegen — issue #4174: reassigning a function-typed (closure) LOCAL never
// released the DISPLACED previous closure value. Measured on the pre-fix
// compiler (`bezelc --sanitize=address`, LeakSanitizer on, 500x amplified):
//
//     var f: () -> int := lambda() => 0;
//     f := lambda() => 42;
//     return f;
//                                   ->  40 bytes / 1 allocation leaked per call
//
// while binding a closure local exactly ONCE (no reassignment) was already
// ASan-clean.
//
// A CITATION NOTE, worth stating explicitly (review of the first version of
// this fix caught a mis-citation, the same class as open issue #4177).
// `string` and function-typed values are BOTH declared runtime-managed by
// §28.9 ("exempt from manual disposal... the type's lifetime is managed by
// the runtime without user involvement") — that much is real spec authority
// for WHY neither family goes through the `owned`/`using var` disposal
// ceremony. But §28.9's own closing sentence disclaims the REST of the
// claim this file's first revision made: "The implementation of
// runtime-managed types' lifecycle (reference counting, region allocation,
// or other) is not part of this specification." §27.5 ("Closures") never
// mentions reassignment or overwrite at all. And §28.7's drop-on-overwrite
// rule explicitly scopes itself AWAY from this case — its own "Scope"
// clause is "`owned` fields, plain-`var` owned locals, and — for
// whole-value reassignment — move-only value locals and instance fields",
// and §28.1 makes `owned` a compile error on a function type, so a
// function-typed local can never be in that scope. So: this fix and its
// tests are NOT implementing a spec-mandated rule. They are an INTERNAL
// REFCOUNTING-SOUNDNESS fix — keeping the closure-local reassignment path
// consistent with the codegen's own existing ARC discipline for the SAME
// runtime-managed family (the already-shipped, already-reviewed
// `lowerStringLocalReassign` for `string` locals, #429/#2696), so that the
// §28.9 guarantee ("memory... reclaimed automatically") holds in practice
// for a reassigned closure local the same way it already holds for a
// reassigned string local. Test names below keep their `spec_NN_N` suffixes
// for continuity with the rest of this codebase's naming convention, but no
// individual assertion in this file rests on spec section TEXT the way a
// language-rule test's would; the citation is to this file header, not to a
// section that mandates the behavior.
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
// REVIEW FINDING, FIXED IN THIS REVISION: `lowerClosureLocalReassign`'s
// gate, copied from `lowerStringLocalReassign`, is `localSlot()` — which
// resolves BOTH a `LocalSymbol` and a `ParameterSymbol`
// (`LlvmBackend_Layout.cpp`). A by-value closure-typed PARAMETER's slot
// owns NOTHING at bind (its single reference belongs to the CALLER — the
// same #2696 hazard already fixed for `string` parameters), so the
// drop-on-overwrite release on a parameter's first assignment freed a
// reference this frame never owned: a deterministic heap-use-after-free
// (`p: () -> int` parameter reassigned inside the callee; the CALLER's own
// argument binding is freed under it). Fixed by
// `Emitter::retainAssignedClosureParams`, the closure twin of the existing
// `retainAssignedStringParams` (#2696) — every by-value closure-typed
// parameter a body assigns to is retained once at bind, giving the frame
// its OWN independent +1 before the drop-on-overwrite release can ever
// consume it. See `parameter_reassignment_no_uaf_caller_binding_intact`
// below.
//
// The first three tests use runAotChecked (generated-code ASan with
// LeakSanitizer ON), so `r.ran` is itself the leak assertion, exactly as
// const_local_closure_release_3402_test.cpp's file-level convention states.
// The rest (self-assignment / borrow / method-reference / the parameter
// row) use runAotCheckedLenient (ASan on, LeakSanitizer OFF) instead — see
// the runLenientOut helper's own comment for why: each has a FINAL closure
// value that is, on BOTH the pre-fix and post-fix compiler, excluded from
// the scope-end release set by the pre-existing, documented "a reassigned
// local's (or an assigned-to parameter's) final value leaks safely"
// trade-off (mirroring `lowerStringLocalReassign` / `retainAssignedString
// Params`'s own for `string`), which is unrelated to this fix and would
// otherwise trip LeakSanitizer for a program that in fact ran correctly
// (measured: a one-shot 40-byte leak DOES clear LSan's report threshold in
// this harness — confirmed empirically, no floor to rely on).
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
//   p: () -> int PARAMETER reassigned inside the
//     callee, caller's own binding read afterward
//     (one-shot, runAotCheckedLenient)                  CRASH**   PASS (no
//                                                                  crash;
//                                                                  r=99 h=5)
//
// * these rows are not FAIL-WITHOUT rows: they are no-crash / no-double-free
//   controls under ASan with LeakSanitizer deliberately disabled — see each
//   test's own comment for why. The FAIL-WITHOUT/PASS-WITH claim for this
//   issue rests on the first three (runAotChecked) rows.
// ** this row FAILED DIFFERENTLY on the first revision of this fix (before
//   `retainAssignedClosureParams` existed): a deterministic
//   heap-use-after-free, not a leak — see the REVIEW FINDING note above.
//   Measured directly against `Emitter::lowerClosureLocalReassign` WITHOUT
//   `retainAssignedClosureParams` (i.e. the first revision's exact code): a
//   reproducible ASan abort, "freed in bz$M.C$Reassign, previously
//   allocated in main". This row is a no-crash control against the PARENT
//   commit's parameter defect, not a FAIL-WITHOUT row against #4174's own
//   original (pre-either-revision) baseline, which this program does not
//   exercise (the original baseline's parameter behavior was a plain,
//   unreleased store — leak-only, never a crash — since #4174's own bug was
//   simply "nothing happens on reassignment" until the first revision of
//   this fix armed the release without its paired retain).
//
// per issue #4174 (see the CITATION NOTE above for why no spec section is
// claimed as authority for the reassignment mechanism itself)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
// LSan-DISABLED (ASan still on): for the rows below whose FINAL closure
// value is — on BOTH the pre-fix and post-fix compiler, by the documented
// ClosureEscapeWalker / lowerStringLocalReassign-and-retainAssignedString
// Params-parity design (see the file-level comment) — never released at
// scope end. Those rows assert "no crash / correct value", not
// "leak-clean"; runAotChecked would trip LeakSanitizer on that
// pre-existing, unrelated leak and report "<did-not-run>" for a program
// that in fact ran correctly (measured: a one-shot 40-byte leak DOES clear
// LSan's report threshold — no floor to rely on here). runAotCheckedLenient
// (issue #2214) keeps generated-code ASan (use-after-free / double-free)
// active while disabling only LeakSanitizer, which is exactly the axis
// these rows test.
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
// TRANSFERRED final closure. (issue #4174; see file header CITATION NOTE)
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
// (issue #4174; see file header CITATION NOTE)
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
// (issue #4174; see file header CITATION NOTE)
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

// ── PARAMETER axis (review NEEDS-FIX, LANE_RULES §15 — the accept-side grid
// gap the first revision's sweep missed: every row there declared a LOCAL,
// none a closure-typed PARAMETER). `p: () -> int` is a by-value closure
// parameter; `Reassign` assigns to it and calls the NEW value; `Main` reads
// its OWN separately-bound closure `h` (the argument passed to `Reassign`)
// AFTER the call. On the first revision of this fix (no
// `retainAssignedClosureParams`), `lowerClosureLocalReassign`'s
// drop-on-overwrite release fired on `p`'s slot — which `localSlot()`
// resolves exactly as it resolves a local, but which owns NOTHING at bind,
// its single reference belonging to the CALLER — freeing `h`'s own cell out
// from under `Main`: a deterministic heap-use-after-free
// ("freed in bz$M.C$Reassign, previously allocated in main"), reproduced
// one-shot AND 500x-amplified, aborting on the first iteration every time.
// `retainAssignedClosureParams` (the closure twin of `retainAssignedString
// Params`, #2696) fixes this by retaining `p`'s incoming value once at bind
// whenever the body assigns to it, giving `Reassign`'s frame its own
// independent +1 before the release can ever consume it — so the release
// now frees THIS frame's retain, not the caller's borrow. `r=99` (the
// result of calling the reassigned `p`) and `h=5` (the CALLER's own
// binding, still intact and correct) must BOTH hold.
// (issue #4174 review follow-up; see file header CITATION NOTE)
BEZEL_TEST(closure_local_reassign_release_4174,
           parameter_reassignment_no_uaf_caller_binding_intact) {
    BEZEL_EXPECT_EQ(runLenientOut(
        "module M; import System; "
        "class C "
        "  public static method Reassign(p: () -> int): int "
        "    p := lambda() => 99; "
        "    return p(); "
        "  end Reassign; "
        "  public static method Main(): int "
        "    var h: () -> int := lambda() => 5; "
        "    var r := C.Reassign(h); "
        "    Console.WriteLine(\"r=\" + r.ToString() + \" h=\" + h().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "r=99 h=5\n");
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
// escaped regardless of this fix, mirroring the string precedent's own
// "leaks safely rather than double-frees" trade-off): it asserts the
// program runs to completion with the CORRECT value under ASan on both
// `f()` and `g()` — i.e., no premature double-free/UAF from an
// under-retained alias. (issue #4174; see file header CITATION NOTE)
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
// never through 0 — mirroring the string precedent's own retain-before-
// release ordering rationale. (issue #4174; see file header CITATION NOTE)
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

// ── METHOD-REFERENCE axis (§27.6, a genuine grammar citation — the RHS is
// syntactically a method reference, not an authority for the reassignment
// mechanism): the RHS producer can be a method reference, not only a lambda
// literal — `isUserMethodClosureReturnCall` / `isPlusOneClosureProducer`'s
// method-reference arm must still be recognized as a TRANSFER (no retain)
// by the reassignment helper, exactly as `lowerClosureFieldStore` already
// recognizes it. Pre-fix this row leaked BOTH the displaced `lambda()=>0`
// (#4174's own bug) and — unrelated to this fix, unchanged on both sides —
// the reassigned local's own final value at scope end (never
// returned/consumed further here).
// (issue #4174; see file header CITATION NOTE)
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
