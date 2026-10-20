// Codegen / refcounting — ASSIGNING to a `string` PARAMETER released the
// CALLER's string. The drop-on-overwrite `bz_string_release` that `s := e` (and
// `s += e`) emits is correct for a LOCAL, whose slot was given a +1 at bind; a
// PARAMETER's slot was given nothing, so the FIRST assignment freed a reference
// this frame never owned. Issue #2696 (the recorded residual of #2689).
//
// Spec: §28.1 "Where the `owned` modifier can appear" — "On any other type — a
// primitive, `string`, a function type, an ordinary record, an enum, a
// non-owning inline array, or the unit type — `owned` is a compile error: such
// values carry no disposal obligation to transfer (§28.9)" — so a `string`
// parameter is ALWAYS an unmodified borrow whose one reference belongs to the
// CALLER; §28.9 "Runtime-managed types" (every co-owner takes exactly one
// reference and releases exactly one); §4.6 (the string `+`/`+=` concat forms).
// Spec revision v0.240.
//
// THE BUG, AND WHY IT IS NOT THE LAMBDA. The issue was filed against a
// reassigned parameter CAPTURED BY A LAMBDA, but the capture is not required:
// the same program with no lambda at all SEGVs identically, and the crash frame
// is `bz_string_release <- main` — the CALLER releasing the argument it still
// owns — not `bz$lam$N$envdtor`:
//
//     AddressSanitizer: SEGV in __asan::Allocator::Deallocate
//       #2 bz_string_release   #3 main
//
// Emitted IR for `method Two(s: string)` with `s := s + "-x";`, pre-fix:
//
//     store ptr %0, ptr %3            ; the parameter slot — NO retain
//     ...
//     %strl.old = load ptr, ptr %3    ; = the CALLER's string
//     store ptr %str.obj, ptr %3
//     call void @bz_string_release(ptr %strl.old)   ; <- unpaired
//
// WHICH OWNERSHIP MODEL GOVERNS A REASSIGNED PARAMETER. The borrow model still
// does, and nothing is inherited from anywhere: the escape machinery never sees
// parameters. collectStringReleaseEligible collects `const ast::LocalDecl*`
// candidates only, and armStringLocalRefcount — the sole registrar of a
// scope-end Disposal{stringRelease} — is reached only from lowerLocalDecl. A
// parameter is in none of stringBorrowAll_ / stringReleaseEligible_ /
// stringBorrowRetainEligible_, so no scope-end release of it exists to be
// suppressed, and #2689's premise for a CAPTURED parameter is untouched. The
// defect was that lowerStringLocalReassign's gate, localSlot(), admits a
// ParameterSymbol (LlvmBackend_Layout.cpp) and so applied the LOCALS model's
// drop-on-overwrite release to a slot the frame did not own.
//
// THE FIX retains an assigned-to `string` parameter at bind, giving its slot the
// independent +1 the release then consumes — exactly the treatment
// armStringLocalRefcount gives a BORROW-initialized string local. It is purely
// ADDITIVE to the emitted refcount traffic, which is why it cannot introduce an
// over-release anywhere.
//
// ⚠ RESIDUAL, BY DESIGN: no scope-end release is registered for the parameter
// slot, so the LAST value assigned to it leaks (one string per call) unless that
// value is a literal/sentinel. EVERY case below is therefore written to end on a
// literal — either by assigning one outright (A, C, F) or by assigning one after
// the concat that the drop-on-overwrite release then reclaims (B, D, G) — so all
// seven run under `runAotChecked` with LSan as a live oracle in both directions.
// See the follow-up issue for the residual itself.
//
// WHERE THE RETAIN IS EMITTED, AND WHY THAT IS A GATE QUESTION. Both releases it
// pairs are gated on `currentFn_ == stringReleaseEligibleFn_`, and that token is
// assigned in exactly ONE place in the backend — `emitFunctionBody`. So the
// releases fire only in a body lowered there. `retainAssignedStringParams`
// carries the SAME gate, which is what keeps the two predicates in agreement:
//   * ordinary method body      -> gate ON  : release fires, retain pairs it.
//   * COMPILED constructor      -> gate OFF : `emitConstructorBody` sets
//     currentFn_ but never the token, so the assignment is a plain store and NO
//     retain is emitted. Case (F) pins this: a retain here would pair nothing
//     and leak one string per constructor call.
//   * INLINED constructor       -> gate ON  : `inlineCtorBody` lowers into the
//     ENCLOSING function and never touches currentFn_, so the release DOES fire.
//     Case (G) pins this — pre-fix it was a live use-after-free.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 7 absorbing rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics and returned from the row on it —
// the definition scripts/check_toolchain_skip_surface.py uses for an
// ABSORBING site. It was also a silent vacuous pass: on a box whose clang
// cannot link the ASan binary, every one of those rows returned WITHOUT
// asserting, and the suite reported a clean PASS having compiled and run
// nothing.
//
// Each guard becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position:
// the skip is now LOUD — the row FAILS — instead of being absorbed by a
// guard that reports a clean pass.
// Every substantive assertion in every row — `r.ran` and the exitCode pins —
// stays exactly where it was, and the local predicate is deleted so no
// absorbing shape survives. The guard stays a SEPARATE statement from the
// predicate on purpose: inlining it into an `if` recreates the shape the
// predicate exists to end.

// (A) The minimal shape: ONE assignment to a `string` parameter, no lambda
// anywhere. Pre-fix this SEGV'd in `main` when the caller released the argument
// the callee had already freed. The assigned value is a LITERAL (an rc
// sentinel), so the residual "last assigned value leaks" does not apply and LSan
// is a live oracle here. `n` is read BEFORE the assignment and returned, so a
// premature free corrupts the exit code independently of the sanitizer.
//
// ⚠ Every string is built at RUNTIME (`$"v-{i}"`): `"ab" + "cd"` constant-folds
// to an rc-sentinel on which `bz_string_release` no-ops, the trap that made
// three earlier probes in this family vacuous.
//
// Value: sum over i in 1..20 of (len("v-{i}") + len("lit")) = 71 + 60 = 131.
const char* kAssignLiteralToStringParam =
    "module M; class C "
    "  public static method Two(s: string): int "
    "    var n := s.Length; "
    "    s := \"lit\"; "
    "    return n + s.Length; "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Two($\"v-{i}\"); "   // runtime-built, NOT folded
    "    end; "
    "    return total; "                            // 131
    "  end Main; "
    "end C;";

// (B) THE UNDER-CORRECTION GUARD, and the strongest case in the file. TWO
// assignments: the first displaces the caller's BORROW (must NOT be released),
// the second displaces the concat result the frame now OWNS (must be released).
// Ending on a literal makes the program fully balanced, so LSan fails it in BOTH
// directions:
//   * pre-fix — SEGV on the first assignment's unpaired release;
//   * had the bug been "fixed" by deleting the drop-on-overwrite release
//     instead of adding the missing retain, the concat result displaced by the
//     second assignment would leak and LSan would fail it.
// Value: sum over i in 1..20 of (len("v-{i}") + 2 + len("zz"))
//        = (71 + 40) + 40 = 151.
const char* kTwoAssignmentsToStringParam =
    "module M; class C "
    "  public static method Two(s: string): int "
    "    s := s + \"-x\"; "
    "    var n := s.Length; "
    "    s := \"zz\"; "
    "    return n + s.Length; "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Two($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 151
    "  end Main; "
    "end C;";

// (C) The issue's shape: the parameter is REASSIGNED and then captured by two
// lambdas. Pre-fix: `SEGV in __asan::Allocator::Deallocate`, frames
// `bz_string_release <- main`.
//
// ⚠ THE ASSIGNED VALUE IS A LITERAL, NOT `s + "-x"` AS IN THE ISSUE, and that is
// deliberate on BOTH counts. (1) A literal is an rc sentinel, so the residual
// last-value leak does not apply and this can run under LSan. (2) The issue's
// verbatim `s := s + "-x"` form CANNOT be pinned here: under LSan the residual
// leak owns the signal, and on the PLAIN path it is VACUOUS — verified, it
// passes pre-fix, because the freed string is only touched by the caller AFTER
// `Two` has already computed its result, and glibc's free of an rc-corrupted
// block happens not to fault. A test that passes either way is worse than no
// test. The captures are exercised for real: `f1`/`f2` are invoked and their
// lengths summed, and `n` pins the caller's string BEFORE the assignment.
//
// Value: sum over i in 1..20 of (len("v-{i}") + 2*4) = 71 + 160 = 231.
const char* kReassignThenCaptureTwice =
    "module M; class C "
    "  public static method Two(s: string): int "
    "    var n := s.Length; "
    "    s := \"abcd\"; "
    "    var f1 := lambda(k: int) => do return s; end; "
    "    var f2 := lambda(k: int) => do return s; end; "
    "    return n + f1(1).Length + f2(2).Length; "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Two($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 231
    "  end Main; "
    "end C;";

// (D) The §4.6 COMPOUND form of the same defect. `s += t` on a parameter takes a
// different code path from `s := e` (the string `+=` branch in
// LlvmBackend_Stmts.cpp, whose release-old is gated on the same localSlot()
// test), and it SEGV'd identically pre-fix — the one retain at bind pairs both.
// The trailing `s := "zz"` releases the frame-owned concat result, so this case
// is leak-free too and LSan stays a live oracle (same construction as (B)).
// Value: sum over i in 1..20 of (len + (len+2) + 2) = 71 + 111 + 40 = 222.
const char* kCompoundAssignToStringParam =
    "module M; class C "
    "  public static method Two(s: string): int "
    "    var n := s.Length; "
    "    s += \"-x\"; "
    "    var m := s.Length; "
    "    s := \"zz\"; "
    "    return n + m + s.Length; "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Two($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 222
    "  end Main; "
    "end C;";

// (E) OVER-CORRECTION GUARD — a `string` parameter that is NEVER assigned must
// get NO retain at bind, or its caller's string leaks once per call. This is
// #2689's own case (A), green before and after by design; it fails here if the
// body walk that gates the new retain ever admits a non-assigning body.
// Value: 2 * 71 = 142.
const char* kCapturedStringParamNeverAssigned =
    "module M; class C "
    "  public static method Two(s: string): int "
    "    var f1 := lambda(k: int) => do return s; end; "
    "    var f2 := lambda(k: int) => do return s; end; "
    "    return f1(1).Length + f2(2).Length; "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Two($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 142
    "  end Main; "
    "end C;";

// (F) COMPILED-CONSTRUCTOR GATE GUARD. `emitConstructorBody` sets `currentFn_`
// but never `stringReleaseEligibleFn_`, so in a compiled constructor BOTH
// drop-on-overwrite releases are gated off and `s := "lit"` is a plain store.
// The retain must therefore NOT be emitted here — one that was would mint a +1
// pairing nothing and leak the incoming string once per constructor call. The
// class is non-generic so `new Holder(...)` takes the compiled path, and the
// assigned value is a literal so the program is otherwise leak-free: if the
// retain ever comes back, these 20 caller strings leak and LSan fails.
// Value: 20 * len("lit") = 60.
const char* kCompiledCtorAssignsStringParam =
    "module M; class Holder "
    "  public N: int; "
    "  public constructor(s: string) "
    "    s := \"lit\"; "
    "    N := s.Length; "
    "  end constructor; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var h := new Holder($\"v-{i}\"); "
    "      total := total + h.N; "
    "    end; "
    "    return total; "                            // 60
    "  end Main; "
    "end C;";

// (G) INLINED-CONSTRUCTOR CASE — the live one. `inlineCtorBody` lowers a
// constructor body into the ENCLOSING function without touching `currentFn_`,
// so when that enclosing body is the analyzed one the release DOES fire on the
// constructor's own `string` parameter. Pre-fix this SEGV'd exactly like the
// method case (`bz_string_release <- main`) and the retain was absent, because
// it was being emitted in the COMPILED constructor prologue instead — the one
// place it does nothing. A GENERIC class is what selects the inline path.
// The trailing `s := "zz"` reclaims the concat, keeping the case leak-free.
// Value: sum over i in 1..20 of ((len("v-{i}") + 2) + 2) = 111 + 40 = 151.
const char* kInlinedCtorAssignsStringParam =
    "module M; class Box<T> "
    "  public N: int; "
    "  public constructor(s: string, v: T) "
    "    s := s + \"-x\"; "
    "    N := s.Length; "
    "    s := \"zz\"; "
    "    N := N + s.Length; "
    "  end constructor; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var b := new Box<int>($\"v-{i}\", i); "
    "      total := total + b.N; "
    "    end; "
    "    return total; "                            // 151
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) #2696 — one assignment to a `string` parameter no longer releases the
// caller's borrow. No lambda involved. §28.1 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_string_param_reassign_borrow_2696,
           assigning_string_param_does_not_release_caller_borrow_spec_28_1) {
    JitResult r = runAotChecked(kAssignLiteralToStringParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);          // no over-release AND no leak
    BEZEL_EXPECT_EQ(r.exitCode, 131);  // the pre-assignment length read back
}

// =====================================================================
// (B) The second assignment must STILL release the value the frame now owns —
// the guard against "fixing" this by deleting the release. §28.9.
// =====================================================================
BEZEL_TEST(codegen_string_param_reassign_borrow_2696,
           second_assignment_still_releases_frame_owned_value_spec_28_9) {
    JitResult r = runAotChecked(kTwoAssignmentsToStringParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);          // fails on a leak in either direction
    BEZEL_EXPECT_EQ(r.exitCode, 151);
}

// =====================================================================
// (C) The issue's shape — reassigned, then captured twice. §28.1 / §27.5.
// =====================================================================
BEZEL_TEST(codegen_string_param_reassign_borrow_2696,
           reassigned_string_param_captured_twice_no_use_after_free_spec_27_5) {
    JitResult r = runAotChecked(kReassignThenCaptureTwice);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 231);
}

// =====================================================================
// (D) The §4.6 compound form `s += t` — a different lowering path, the same
// unpaired release, paired by the same retain.
// =====================================================================
BEZEL_TEST(codegen_string_param_reassign_borrow_2696,
           compound_assign_to_string_param_no_use_after_free_spec_4_6) {
    JitResult r = runAotChecked(kCompoundAssignToStringParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 222);
}

// =====================================================================
// (E) Over-correction guard: a never-assigned `string` parameter takes no
// retain, so #2689's balanced capture case stays leak-free. §28.1 / §27.5.
// =====================================================================
BEZEL_TEST(codegen_string_param_reassign_borrow_2696,
           never_assigned_string_param_takes_no_retain_spec_28_1) {
    JitResult r = runAotChecked(kCapturedStringParamNeverAssigned);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);          // a spurious retain would leak here
    BEZEL_EXPECT_EQ(r.exitCode, 142);
}

// =====================================================================
// (F) Gate guard: a COMPILED constructor has both releases gated off, so it must
// take NO retain — one here would leak the caller's string per call. §28.9.
// =====================================================================
BEZEL_TEST(codegen_string_param_reassign_borrow_2696,
           compiled_ctor_assigning_string_param_takes_no_retain_spec_28_9) {
    JitResult r = runAotChecked(kCompiledCtorAssignsStringParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);          // a retain gated on nothing would leak
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// =====================================================================
// (G) An INLINED constructor lowers into the enclosing function, so the release
// fires there and the retain must too — the live constructor case. §28.1 / §22.9.
// =====================================================================
BEZEL_TEST(codegen_string_param_reassign_borrow_2696,
           inlined_ctor_assigning_string_param_no_use_after_free_spec_28_1) {
    JitResult r = runAotChecked(kInlinedCtorAssignsStringParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 151);
}
