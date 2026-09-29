// Codegen / §8.6 boxing — issue #3480: `o.Equals(<value-typed argument>)`
// leaked the §8.6 box it minted for the argument — 24 B per call, unbounded in
// a loop. Measured on `d617470e` with the issue's own repro at x500:
// `SUMMARY: AddressSanitizer: 60000 byte(s) leaked in 2500 allocation(s)` —
// 24 B x 5 calls x 500 iterations, all answers correct.
//
// Spec: §8.6 "Boxing" — "When a value type is used where `Object` (or an
// interface implemented by the value type) is expected, the value is boxed …
// Boxed values are runtime-managed (§28.9)", and, for a member call on a box,
// "it borrows the payload for the call's duration". §8.1 declares the single
// universal `Equals(other: Object?)`, so §5.1 boxes a value-typed argument at
// that `Object` location. §8.3 fixes what a box's `Equals` does: compare
// payloads. §28.9 lists "Boxed value types" as "exempt from manual disposal"
// and carves the mechanism out — "The implementation of runtime-managed types'
// lifecycle (reference counting, region allocation, or other) is not part of
// this specification." Spec revision v0.306.
//
// THE DEFECT, AND WHY THE OBVIOUS PATCH IS WRONG. #3443 gave the universal
// `Equals` argument its §8.6 boxing (before it, every value-typed argument
// dropped the whole call); the fresh box was deliberately NOT discharged,
// because releasing it right after the dispatch WAS implemented and measured to
// produce a heap-use-after-free. The callee is a vtable dispatch, so it is not
// statically known, and a user `Equals` override may store its borrowed
// `Object?` parameter into a field taking no `bz_box_retain`
// (`collectBoxStorageFields` cannot classify a field whose only assignment
// source is an `Object?` PARAMETER). That storing program is ADMITTED by the
// spec, not a defect in the test: v0.306 §28.14's *deliberate asymmetry*
// paragraph — which refers to §28.3's borrowed-field rule — says "a
// parameter-sourced object borrow may be stored into a field under §28.3's
// borrowed-field rule — a structural check whose residual is the caller's
// trust". Case (C) below is that exact program, kept as a control.
//
// WHAT THE FIX DOES INSTEAD. It discharges the argument box exactly where the
// dispatch target is PROVABLY a compiler-generated box-equals thunk: when the
// receiver is itself a §8.6 box. A box's word 0 is a box vtable, and every box
// vtable's `kEqualsSlot` is filled with `bz$box$eq` / `bz$boxeq$<T>` or an
// inline-array / range / span variant, each of which reads `other`'s word 0
// and payload and returns `i1` without storing it. The invariant is that the
// user method never receives the BOX POINTER — not that none is reachable,
// which is false: `emitEnumEquals` does dispatch a user `Equals(E)` from the
// boxed path, and what protects this fix is that function's
// `selfTy->isIntegerTy() && otherTy->isIntegerTy()` gate, which hands over the
// unboxed integer payload and declines an `Object?` signature. Where the receiver is a user class
// reference the box is left to leak, which is #3480's own recorded
// conservative direction: "the compiler's escape analysis conservatively leaks
// rather than guessing".
//
// WARNING Every case is LOOP-AMPLIFIED (x200). A one-shot LSan row is not an
// oracle here: a single 24-byte box is routinely still reachable from a live
// stack slot or register at exit, so the row would pass on BOTH sides of the
// fix.
//
// MECHANICAL FAIL -> PASS (`--sanitize=address`, per case):
//   (A) 24000 B / 1000 allocs -> clean   (the issue's own repro, 5 call shapes)
//   (B)  9600 B /  400 allocs -> clean   (an interface-typed receiver)
//   (D)  9600 B /  400 allocs -> clean   (value receiver, NULLABLE argument)
//   (C) over-release control: leaks before AND after; goes red on the
//       EXIT CODE if a release is ever armed here (ASan stays silent)
// The PLAIN (unsanitized) exit code is identical before and after in every
// case — a leak is invisible without a sanitizer.

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
// (A) THE ISSUE'S OWN REPRO, loop-amplified: five argument shapes whose
// static type is a value type, so §8.6 mints a box for each — an `int`
// literal, an `int` local, the unit value, a fresh record, and the `?.`
// spelling. Answers per iteration: `o` holds 7, so only `o.Equals(7)` and
// the record comparison are True; `t` therefore lands on 400.
// §8.6 / §8.3 / §28.9.
// =====================================================================
const char* kValueTypedArguments =
    "module M; "
    "record Pt(X: int, Y: int); "
    "class C "
    "  public static method Main(): int "
    "    var o: Object := 7; "
    "    var on: Object? := 7; "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    while i < 200 do "
    "      if o.Equals(7) then t := t + 1; end if; "
    "      if o.Equals(i) then t := t + 1; end if; "
    "      if o.Equals(()) then t := t + 1; end if; "
    "      var ro: Object := new Pt(1, 2); "
    "      if ro.Equals(new Pt(1, 2)) then t := t + 1; end if; "
    "      if (on?.Equals(3) ?? false) then t := t + 1; end if; "
    "      i := i + 1; "
    "    end while; "
    "    if t = 401 then return 7; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// =====================================================================
// (B) AN INTERFACE-TYPED RECEIVER holding a boxed record — the other face
// of the same dispatch, and a different route into `pureBoxBindings_`
// (§8.6's "or an interface implemented by the value type" boxing target,
// not `Object`). Both arguments are boxed: a value literal and a fresh
// record. §8.6 / §8.3 / §28.9.
//
// ⚠ This row REPLACED a value-semantics-receiver row (`p.Equals(new Pt(…))`
// on a record local) that was written first and then MEASURED VACUOUS: it is
// leak-clean on the pre-fix build too, because a record's homogeneous
// auto-generated `Equals(Pt)` wins resolution and no box is minted at all.
// Its NON-nullable value-argument twin (`p.Equals(5)`) is vacuous for the
// adjacent reason — #3538's `isEqualsArgStaticallyFalse` settles the answer
// from the static types and never reaches the boxing path. Case (D) below is
// the shape at that same face which is NOT vacuous.
// =====================================================================
const char* kInterfaceReceiver =
    "module M; "
    "interface IFace "
    "  method Tag(): int; "
    "end IFace; "
    "record R(v: int) : IFace "
    "  public method Tag(): int return v; end Tag; "
    "end R; "
    "class C "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    while i < 200 do "
    "      var f: IFace := new R(1); "
    "      if f.Equals(5) then t := t + 1; end if; "
    "      if f.Equals(new R(1)) then t := t + 1; end if; "
    "      i := i + 1; "
    "    end while; "
    "    if t = 200 then return 7; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// =====================================================================
// (C) CONTROL — #3480'S OWN UAF REPRO, which the issue required be in the
// regression test: "the UAF repro above must be in the regression test, and
// it must FAIL on a build with the naive release." The callee STORES its
// borrowed `Object?` parameter, taking no retain, and the caller reads the
// stored box back after the call. A post-dispatch `bz_box_release` here
// frees a box the callee still holds.
//
// ⚠ WHAT MAKES THIS ROW GO RED IS THE EXIT CODE, NOT THE SANITIZER. The
// counterfactual was run: forcing `knownBoxReceiverShape` to true, so the
// release IS armed here, makes this program exit 1 with **no ASan diagnostic
// at all** — the freed box reads back silently wrong, `seen` never reaches
// 200, and the plain row below fails for the same reason with no sanitizer
// in the picture. #3480's own text predicted a visible heap-use-after-free;
// at this loop shape the generated-code ASan does not report one, which is
// #2633's point exactly. Do not delete `BEZEL_EXPECT_EQ(r.exitCode, 7)` on
// the theory that the sanitizer covers it.
//
// The LENIENT checked oracle is still the right harness — LSan off so the
// deliberate residual leak does not mask the value assertion, generated-code
// ASan left ON because an over-release elsewhere could land as a fault — but
// the value assertion is what is load-bearing here.
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
    "      var b: bool := o.Equals(5); "
    "      if b then seen := seen + 100; end if; "
    "      if k.S <> null then "
    "        if k.S! is int then seen := seen + 1; end if; "
    "      end if; "
    "    end; "
    "    if seen = 200 then return 7; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// =====================================================================
// (D) A VALUE-SEMANTICS RECEIVER WITH A NULLABLE-VALUE ARGUMENT — the face
// `lowerValueSemUniversalEquals` actually mints for. This row exists
// because the first draft of this file asserted the opposite: that the
// release added at that site was "a no-op today". It is not.
//
// That function's own header names the argument shapes reaching it: "an
// `Object`, an interface, a class, or ANY NULLABLE FORM". A nullable VALUE
// argument is not settled by #3538's `isEqualsArgStaticallyFalse`, and it
// DOES mint — `boxForTarget`'s nullable arm produces a box through its PHI
// (LlvmBackend_Layout.cpp). MEASURED on the fixed tree with ONLY the line
// `if (argMint) builder_.CreateCall(relFn, {argMint});` disabled inside
// `lowerValueSemUniversalEquals`: 9600 B leaked in 400 allocations
// (2 boxes x 200 iterations); with the line, clean. So that line is a live
// fix, and this row is what stops it being deleted as dead code.
//
// The release is sound at this face BY CONSTRUCTION rather than by
// analysis: the receiver's box is minted right there from a statically
// known `recvTy`, so the vtable the dispatch reads is
// `boxVtableFor(recvTy)` and its `kEqualsSlot` holds a compiler thunk.
// §8.6 / §8.1 / §7.5 / §28.9.
// =====================================================================
const char* kValueReceiverNullableArgument =
    "module M; "
    "record Pt(X: int, Y: int); "
    "class C "
    "  public static method Main(): int "
    "    var p: Pt := new Pt(1, 2); "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    while i < 200 do "
    "      var pq: Pt? := new Pt(1, 2); "
    "      if p.Equals(pq) then t := t + 1; end if; "
    "      var ni: int? := 5; "
    "      if p.Equals(ni) then t := t + 1; end if; "
    "      i := i + 1; "
    "    end while; "
    "    if t = 200 then return 7; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

}  // namespace

// ---------------------------------------------------------------------
// (A) the issue's own repro.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_object_equals_value_arg_box_reclaim_3480,
           value_typed_argument_boxes_are_released_spec_8_6) {
    JitResult r = runAotChecked(kValueTypedArguments);
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: 24000 B / 1000 allocations
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_object_equals_value_arg_box_reclaim_3480,
           value_typed_argument_plain_value_invariant_spec_8_3) {
    // NOT a #3480 oracle — a leak is invisible without a sanitizer. It pins
    // that the fix did not change any of the five answers, i.e. that no box
    // was freed before `bz$boxeq` read it.
    JitResult r = runAot(kValueTypedArguments);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------
// (B) interface-typed receiver holding a boxed record.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_object_equals_value_arg_box_reclaim_3480,
           interface_receiver_argument_boxes_released_spec_8_6) {
    JitResult r = runAotChecked(kInterfaceReceiver);
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: 9600 B / 400 allocations
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_object_equals_value_arg_box_reclaim_3480,
           interface_receiver_plain_value_invariant_spec_8_3) {
    JitResult r = runAot(kInterfaceReceiver);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------
// (C) CONTROL: #3480's own UAF repro. The class-receiver path must keep
// its conservative leak. Lenient checked oracle — LSan off, ASan on.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_object_equals_value_arg_box_reclaim_3480,
           class_receiver_argument_box_is_not_released_spec_28_3) {
    JitResult r = runAotCheckedLenient(kClassReceiverUafGuard);
    BEZEL_EXPECT_TRUE(r.ran);
    // THE oracle: with a release armed here the freed box reads back wrong
    // and `seen` never reaches 200 (measured; ASan stays silent).
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_object_equals_value_arg_box_reclaim_3480,
           class_receiver_plain_value_invariant_spec_28_3) {
    JitResult r = runAot(kClassReceiverUafGuard);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------
// (D) value-semantics receiver, NULLABLE-value argument — the face the
// site-2 release actually fires for.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_object_equals_value_arg_box_reclaim_3480,
           value_receiver_nullable_argument_box_released_spec_8_6) {
    JitResult r = runAotChecked(kValueReceiverNullableArgument);
    // 9600 B / 400 allocations with the site-2 release disabled (measured).
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_object_equals_value_arg_box_reclaim_3480,
           value_receiver_nullable_argument_plain_invariant_spec_7_5) {
    JitResult r = runAot(kValueReceiverNullableArgument);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
