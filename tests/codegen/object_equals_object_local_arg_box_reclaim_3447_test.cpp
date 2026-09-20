// Codegen / §8.6 boxing — issue #3447: `oi.Equals(q)` on two `Object` locals
// leaked one 24-byte box per call. Measured on `d617470e` with the issue's own
// repro at x500: `SUMMARY: AddressSanitizer: 12000 byte(s) leaked in 500
// allocation(s)` — exactly one box per iteration, unbounded in a loop.
//
// Spec: §8.6 "Boxing" — "When a value type is used where `Object` (or an
// interface implemented by the value type) is expected, the value is boxed: a
// heap-allocated wrapper holds the value. Boxed values are runtime-managed
// (§28.9)"; and, of a member call on a box, "it borrows the payload for the
// call's duration". §28.9 "Runtime-managed types" lists "Boxed value types"
// among the types "exempt from manual disposal" and carves the mechanism out:
// "The implementation of runtime-managed types' lifecycle (reference counting,
// region allocation, or other) is not part of this specification." §8.3 fixes
// what a box's `Equals` DOES: it compares payloads. Spec revision v0.306.
//
// THE DEFECT. Exactly ONE of the two boxes leaked, and which one was decided
// by POSITION: `oi` is the call's RECEIVER, which `BoxEscapeWalker` already
// admits as a call-bounded borrow (#2060), so its scope-end `bz_box_release`
// stayed armed; `q` is the call's ARGUMENT, which `walkArgs` classified as an
// ESCAPE, so `q` never joined `boxReleaseEligible_` and its box was never
// released. Decomposed on the pre-fix build (x100 each):
//     `var oi …; var q …;` with neither passed        -> clean
//     `oi.Equals(oi)`  (the local in BOTH positions)   -> 2400 B / 100 allocs
//     `oi.Equals(q)` and `q.Equals(oi)`                -> 4800 B / 200 allocs
//
// WHY THE RELAXATION IS SOUND — AND WHY IT IS KEYED ON THE RECEIVER. The
// argument position is an escape in general because the callee is a VTABLE
// DISPATCH and a user `Equals` override may store its argument: the spec
// (v0.306, §28.14's *deliberate asymmetry* paragraph, which refers to §28.3's
// borrowed-field rule) states that "a parameter-sourced object borrow may be
// stored into a field under §28.3's borrowed-field rule — a structural check
// whose residual is the caller's trust", so that program is ADMITTED, and
// #3480 measured a heap-use-after-free when the caller released anyway. The
// relaxation is therefore keyed on proving the RECEIVER is a §8.6 box: a
// box's word 0 is a box vtable, every box vtable's `kEqualsSlot` is a
// compiler-generated thunk (`bz$box$eq` / `bz$boxeq$<T>` and the
// inline-array / range / span variants), and none of those thunks stores
// `other` — they read its word 0 and payload and return `i1`.
//
// ⚠ The invariant is "the user method never receives the BOX POINTER", not
// "no user method is reachable from a box vtable" — the latter is false.
// `boxRecordEqualsThunkCore`'s enum arm calls `emitEnumEquals`, which DOES
// dispatch a user-declared `Equals(E)`; the gate that protects this
// relaxation is that function's `selfTy->isIntegerTy() &&
// otherTy->isIntegerTy()` test, which passes the unboxed integer payload and
// declines an `Object?` signature. Case (D) below is the control that pins
// the excluded receiver kind.
//
// WARNING Every case is LOOP-AMPLIFIED (x200). A one-shot LSan row is not an
// oracle here: a single 24-byte box is routinely still reachable from a live
// stack slot or register at exit, so the row would pass on BOTH sides of the
// fix. The counts above are what makes the defect visible.
//
// MECHANICAL FAIL -> PASS (`--sanitize=address`, per case):
//   (A) 7200 B / 300 allocs -> clean   (the issue's own repro)
//   (B) 9600 B / 400 allocs -> clean   (symmetric: each local in both positions)
//   (C) 4800 B / 200 allocs -> clean   (the local as its own argument)
//   (D) over-release control: leaks before AND after; goes red on the
//       EXIT CODE if a release is ever armed here (ASan stays silent)
//   (E) 4800 B / 200 allocs -> clean   (value receiver, Object-local argument)
// The PLAIN (unsanitized) exit code is identical before and after in every
// case — a leak is invisible without a sanitizer, so LSan is the oracle that
// moves and the plain rows pin that nothing is freed early.

// WARNING NO TOOLCHAIN-SKIP GUARD, DELIBERATELY (#3733). An earlier revision
// of this file opened every checked row with a return-early guard over a local
// predicate that tested the result text for the SKIP literal. That is what
// check_toolchain_skip_surface.py calls an ABSORBING SITE: on a box without the
// ASan link toolchain the row returns early and reports a clean PASS, so the
// file proves nothing and says nothing — the same vacuous-green class as a
// one-shot LSan row, and indistinguishable from a real green.
// `scripts/check_toolchain_skip_surface.py` counts those sites and ratchets
// them down; these rows contributed to a +19 rise it caught. The runtime half
// of #3733 — the counter, the printed notice, the shard JSON and
// `run_parallel.sh`'s floor — already makes a missing toolchain loud, so the
// guard was dead weight. With it gone `BEZEL_EXPECT_TRUE(r.ran)` is the first
// assertion in every checked row and a toolchain-less box fails loudly.
// Do not re-add it.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked / …Lenient
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// =====================================================================
// (A) THE ISSUE'S OWN REPRO, loop-amplified. Two `Object` locals, the
// second passed as the argument of §8.1's universal `Equals`. §8.6 / §8.3.
// =====================================================================
const char* kObjectLocalArgument =
    "module M; class C "
    "  public static method Main(): int "
    "    var n: int := 0; "
    "    for var i in 0..<200 do "
    "      var oi: Object := 7; "
    "      var q: Object := 9; "
    "      var same: Object := 7; "
    "      if oi.Equals(q) then n := n + 1; end if; "
    "      if oi.Equals(same) then n := n + 1; end if; "
    "    end; "
    "    if n = 200 then return 7; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// =====================================================================
// (B) SYMMETRIC USE: each local appears once as receiver and once as
// argument. This is the shape a one-pass escape analysis cannot admit —
// each operand's ARGUMENT appearance would disqualify the other's
// RECEIVER — and it is why the classification runs in two phases, the
// first asking only "can anything WRITE this slot". §8.6 / §28.9.
// =====================================================================
const char* kSymmetricBothPositions =
    "module M; class C "
    "  public static method Main(): int "
    "    var n: int := 0; "
    "    for var i in 0..<200 do "
    "      var oi: Object := 7; "
    "      var q: Object := 9; "
    "      if oi.Equals(q) then n := n + 1; end if; "
    "      if q.Equals(oi) then n := n + 1; end if; "
    "    end; "
    "    if n = 0 then return 7; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// =====================================================================
// (C) THE LOCAL AS ITS OWN ARGUMENT: `oi.Equals(oi)`. §8.3's box row
// compares payloads, so the answer is True; the single box must still be
// released exactly once (not twice — the receiver and argument are the
// SAME pointer, and a double release would be a double free, which the
// checked oracle would report as such rather than as a leak).
// =====================================================================
const char* kSelfArgument =
    "module M; class C "
    "  public static method Main(): int "
    "    var n: int := 0; "
    "    for var i in 0..<200 do "
    "      var oi: Object := 7; "
    "      if oi.Equals(oi) then n := n + 1; end if; "
    "    end; "
    "    if n = 200 then return 7; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// =====================================================================
// (D) CONTROL — THE EXCLUDED CASE, which must keep leaking rather than be
// "finished". Same spelling as (A) — a BOUND `Object` local in argument
// position — but the receiver is a user CLASS reference in an `Object`
// slot, so the vtable slot holds the user's `Equals` override, which stores
// its borrowed `Object?` parameter into a field taking no `bz_box_retain`
// (`collectBoxStorageFields` cannot classify a field whose only assignment
// source is an `Object?` parameter). The spec ADMITS that program (v0.306
// §28.3: "a parameter-sourced object borrow may be stored into a field …
// a structural check whose residual is the caller's trust"), so arming
// `q`'s scope-end release here is a use-after-free.
//
// The argument's scope is deliberately NARROWER than the read-back: `q`
// lives in an inner block, and `k.S` is dereferenced after that block has
// ended. A release armed at `q`'s scope end therefore frees the box BEFORE
// the read, which is what makes the row an oracle rather than decoration.
//
// ⚠ WHAT MAKES THIS ROW GO RED IS THE EXIT CODE, NOT THE SANITIZER — and
// that distinction is measured, not assumed. Forcing `knownBoxReceiverShape`
// to return true (the counterfactual in which the release IS armed here) was
// run against this exact program: the process exits 1 with **no ASan
// diagnostic of any kind** — the freed box is read back and silently answers
// wrong, so `seen` never reaches 200. The plain row below fails too, and does
// not need a sanitizer at all. This is #2633's lesson in miniature: the ASan
// oracle did not see a genuine use-after-free, so a later restructuring must
// not delete `BEZEL_EXPECT_EQ(r.exitCode, 7)` on the theory that the
// sanitizer covers it.
//
// The LENIENT checked oracle is still the right harness — LeakSanitizer off
// so the deliberate conservative leak does not mask the value assertion,
// generated-code ASan left ON because a future over-release could land as a
// fault rather than as a wrong answer — but it is the belt, not the braces.
// =====================================================================
const char* kClassReceiverUafGuard =
    "module M; "
    "class K "
    "  public S: Object?; "
    "  public override method Equals(other: Object?): bool "
    "    S := other; "
    "    return false; "
    "  end Equals; "
    "  public override method GetHashCode(): int return 0; end GetHashCode; "
    "end K; "
    "class C "
    "  public static method Main(): int "
    "    var seen: int := 0; "
    "    for var i in 0..<200 do "
    "      using var k := new K(); "
    "      var o: Object := k; "
    "      if i >= 0 then "
    "        var q: Object := 5; "
    "        if o.Equals(q) then seen := seen + 100; end if; "
    "      end if; "
    "      if k.S <> null then "
    "        if k.S! is int then seen := seen + 1; end if; "
    "      end if; "
    "    end; "
    "    if seen = 200 then return 7; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// =====================================================================
// (E) A VALUE-SEMANTICS RECEIVER with a BOUND `Object` local as argument:
// `p.Equals(ob)` on a record local. The receiver is not a local at all
// here — it is a value whose STATIC type is boxable, so this backend
// answers §8.1's `Equals` by boxing the receiver ITSELF from a known
// `recvTy` (`lowerValueSemUniversalEquals`), and the dispatch lands on
// `bz$boxeq$<Pt>` by construction rather than by analysis. That is the
// second arm of the same predicate, and it is what keeps the relaxation a
// statement about the RECEIVER rather than about `Object` locals.
// §8.6 / §8.1 / §8.3.
// =====================================================================
const char* kValueReceiverObjectArgument =
    "module M; "
    "record Pt(X: int, Y: int); "
    "class C "
    "  public static method Main(): int "
    "    var p: Pt := new Pt(1, 2); "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    while i < 200 do "
    "      var ob: Object := 7; "
    "      if p.Equals(ob) then t := t + 1; end if; "
    "      i := i + 1; "
    "    end while; "
    "    if t = 0 then return 7; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

}  // namespace

// ---------------------------------------------------------------------
// (A) the issue's own repro.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_object_equals_object_local_arg_box_reclaim_3447,
           object_local_argument_box_is_released_spec_8_6) {
    JitResult r = runAotChecked(kObjectLocalArgument);
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: 7200 B / 300 allocations
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_object_equals_object_local_arg_box_reclaim_3447,
           object_local_argument_plain_value_invariant_spec_8_3) {
    // NOT a #3447 oracle — a leak is invisible without a sanitizer, so this
    // passes with and without the fix. It pins that the fix did not change
    // the computed answer, i.e. that no box was freed early.
    JitResult r = runAot(kObjectLocalArgument);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------
// (B) each local in BOTH positions.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_object_equals_object_local_arg_box_reclaim_3447,
           symmetric_both_positions_released_spec_8_6) {
    JitResult r = runAotChecked(kSymmetricBothPositions);
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: 9600 B / 400 allocations
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_object_equals_object_local_arg_box_reclaim_3447,
           symmetric_both_positions_plain_value_invariant_spec_8_3) {
    JitResult r = runAot(kSymmetricBothPositions);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------
// (C) the local as its own argument — exactly-once, not twice.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_object_equals_object_local_arg_box_reclaim_3447,
           self_argument_released_exactly_once_spec_8_3) {
    JitResult r = runAotChecked(kSelfArgument);
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: 4800 B / 200 allocations
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_object_equals_object_local_arg_box_reclaim_3447,
           self_argument_plain_value_invariant_spec_8_3) {
    JitResult r = runAot(kSelfArgument);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------
// (D) CONTROL: the class-receiver path must NOT be released. Lenient
// checked oracle — LSan off, generated-code ASan on.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_object_equals_object_local_arg_box_reclaim_3447,
           class_receiver_argument_box_is_not_released_spec_28_3) {
    JitResult r = runAotCheckedLenient(kClassReceiverUafGuard);
    BEZEL_EXPECT_TRUE(r.ran);
    // THE oracle: with a release armed here the freed box reads back wrong
    // and `seen` never reaches 200 (measured; ASan stays silent).
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_object_equals_object_local_arg_box_reclaim_3447,
           class_receiver_plain_value_invariant_spec_28_3) {
    JitResult r = runAot(kClassReceiverUafGuard);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------
// (E) value-semantics receiver, bound `Object` local as the argument.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_object_equals_object_local_arg_box_reclaim_3447,
           value_receiver_object_local_argument_released_spec_8_1) {
    JitResult r = runAotChecked(kValueReceiverObjectArgument);
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: 4800 B / 200 allocations
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_object_equals_object_local_arg_box_reclaim_3447,
           value_receiver_object_local_argument_plain_invariant_spec_8_3) {
    JitResult r = runAot(kValueReceiverObjectArgument);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
