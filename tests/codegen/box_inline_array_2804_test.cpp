// Codegen — §8.6 / #2804: a NON-OWNING inline array `[N]T` used where `Object`
// (or an interface it implements) is expected must be BOXED, like every other
// value type §8.6 does not exclude. It was not, so an `Object`-typed `[3]int`
// held the raw `[3 x i32]` aggregate and every type test on it silently
// answered `false`.
//
// spec §8.6 (v0.240), *Boxing of inline arrays*:
//   "Non-owning inline arrays (`[N]T` and `[N]T?`) are ordinary value types and
//    box per the general rule."
//
// and the general rule it defers to:
//   "When a value type is used where `Object` (or an interface implemented by
//    the value type) is expected, the value is **boxed**: a heap-allocated
//    wrapper holds the value."
//
// and §8.6's ONLY exception, which a non-owning inline array is not in:
//   "Move-only value types — owning inline arrays (`[N owned]T` and
//    `[N owned]T?`) and owning-classified choices ([§35.4]) — cannot be boxed."
//
// The box's three Object-virtual members are fixed by the spec, not chosen here:
//   * §8.3's type-kind table, inline-array row: "Element-by-element value
//     equality ([§15.1.2])" / "Hash combining element hashes in slot order",
//     tied by "`GetHashCode` must satisfy: if `a.Equals(b)` then
//     `a.GetHashCode() = b.GetHashCode()`".
//   * §8.5: "For inline arrays (all forms …) … the type's name — `"[3]int"`-style
//     for an inline array … (element-listing output is deliberately not
//     synthesized — an inline array's elements may be class-typed borrows whose
//     formatting is not the container's business)."
//   * §15.1.2: "inline arrays are not class types and do not implement
//     interfaces" — so the box carries an EMPTY itmap, not an interface table.
//
// THE DEFECT. `isBoxableValueType` ended with an explicit deferral
// (`return false; // inline arrays / unit: deferred`), so `boxForTarget` /
// `boxValue` were no-ops for an inline-array source and `boxDescriptorFor` had
// nothing to resolve for an `is [3]int` target.
//
// THE FIX. §15.1 makes the size part of the type ("`[3]int` and `[4]int` are
// distinct types that do not interconvert"), so unlike every other boxable kind
// an inline array has no stable NAME to key a descriptor by. The box descriptor
// + vtable are minted on demand and cached under the STRUCTURAL identity key
// (`typeRefIdentityKey` / `appendDescKey`), which already encodes ownedness, the
// folded size and the element type (#2001) — so `[3]int`, `[4]int` and
// `[3]string` get three distinct runtime identities. One predicate
// (`inlineArrayBoxable`) feeds the boxability gate, the vtable and the target
// descriptor, so they cannot drift the way #2793's three primitive lists did.
//
// WHAT EACH TEST SHOWS WITHOUT THE FIX — measured by reverting the src change,
// rebuilding and re-running: 9 of these 10 go red.
//   type_test:      0   instead of 65   (no test matched — including `is Object`,
//                                        whose operand must be a pointer)
//   equals_hash:    112 instead of 17   (no Equals matched; ALL THREE hashes were
//                                        the degenerate 0, so `ha = hb`, `ha = hc`
//                                        and `ha = 0` were all true — the exact
//                                        "agrees for the wrong reason" shape the
//                                        two guard bits exist to catch. Codegen
//                                        also reported #2434 three times:
//                                        "initializer expression failed to lower
//                                        to a value … degraded to a zero-
//                                        initialized value")
//   tostring x2:    ""  instead of "[3]int" / "[2]string"
//   element_kinds:  0   instead of 511
//   unbox:          0   instead of 456  (an all-zero array, not the values)
//   both loops:     1   instead of 0    (acc never incremented)
//   sweep:          255 instead of 511  (bit 256 — the inline-array box answering
//                                        `is Object` — was clear)
// So the sweep is a REGRESSION test as well as an over-rejection guard. Exactly
// ONE test is a pure both-ways CONTROL and is labelled as such: the move-only
// guard, which exists to prove the fix did not sweep `[N owned]T` in with the
// non-owning form.
//
// NOT COVERED HERE, deliberately (each filed):
//   * `o as [N]T`: §17.4 says a value-type `as` "produces `T?`", but §15.1 does
//     not admit a nullable inline array (`([3]int)?`) — so a FAILED cast is
//     represented by an all-zero array, indistinguishable from a legitimately
//     zero one. The round-trip on a MATCHING cast is asserted below (it went
//     from all-zeros to the real values); the miss case is not, because the
//     representation the spec would need does not exist. Filed as a spec
//     question rather than answered here.
//     ↳ RESOLVED, #2911 (spec §17.4 "Degenerate value-type targets", v0.241):
//       the spelling is now a compile error — this exact prediction is what the
//       ruling acted on — and the round trip below is re-pointed at the
//       rejection. The 456 value assertion is not deleted but PARKED: §17.4's
//       replacement unbox (the §12.7 `is` narrowing + copy-out) does not lower
//       yet, filed as #3090, and the restore instruction is on the test.
//       ↳ RESTORED, #3090 (spec §12.7 / §8.6 / §17.4, v0.252): the §12.7
//         narrowing + copy-out now lowers, so `kUnboxRoundTrip` is back with its
//         456 assertion in the spelling §17.4 sanctions, ALONGSIDE the `as`
//         rejection. Both halves are kept: the operator stays rejected, and the
//         round trip §8.6 owes is pinned again.
//       The rejection and its accept-controls live in
//       tests/sema/as_degenerate_target_2911_2962_test.cpp.
//   * `$"{ia}"` string interpolation of an inline array still renders the empty
//     string rather than §8.5's "[3]int" — a separate emitValueToString gap,
//     untouched by this change, filed.
//   * An owning `[N owned]T` in an `Object` location is accepted with no
//     diagnostic although §8.6 says it "cannot be boxed" — a missing rejection,
//     filed. The control below pins the codegen half (it is not boxed).
//   * `var o: Object := [1, 2, 3];` (a bare collection literal at an `Object`
//     target) emits malformed IR — measured identically on the UNPATCHED
//     compiler, so it is pre-existing and out of scope here; filed. The sweep
//     below therefore names the array through a typed local.
//   * A boxed ARGUMENT temporary can still leak one wrapper (the #2865 residual
//     the fix's own comment calls "the safe direction"). Measured identically
//     with `int` on the UNPATCHED compiler — 24 bytes there, 32 for a `[3]int`
//     box only because the payload is larger — so it is not introduced here;
//     filed. The two loop tests below hold the shapes that DO release.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

// §8.6 — the issue's cardinal repro, widened over the neighbouring types so the
// fix cannot pass by making the box match EVERYTHING. §15.1: "`[3]int` and
// `[4]int` are distinct types that do not interconvert", so only bits 1 (its own
// type) and 64 (`Object`, the universal root of §8.1) may be set. Want 65.
const char* kTypeTest =
    "module M; class C "
    "  public static method Main(): int "
    "    var a3: [3]int := [1, 2, 3]; "
    "    var o: Object := a3; "
    "    var code := 0; "
    "    if o is [3]int    then code := code + 1;  end; "
    "    if o is [4]int    then code := code + 2;  end; "
    "    if o is [3]string then code := code + 4;  end; "
    "    if o is [2]int    then code := code + 8;  end; "
    "    if o is int       then code := code + 16; end; "
    "    if o is string    then code := code + 32; end; "
    "    if o is Object    then code := code + 64; end; "
    "    Console.WriteLine(code); "
    "    return 0; "
    "  end Main; "
    "end C;";

// §8.3 through an Object receiver: element-by-element equality and an
// order-sensitive element-hash fold. Two separate boxes of the same three values
// are Equals and hash alike (bits 1 + 16); a different element value, a
// different LENGTH, and a boxed int are not equal (bits 2/4/8 must stay clear);
// a different value must not hash the same (bit 32 clear) and the hash must not
// be the degenerate 0 (bit 64 clear) — those last two are the non-vacuity guards
// against an Equals/hash pair that agrees for the wrong reason. Want 17.
const char* kEqualsAndHash =
    "module M; class C "
    "  public static method Main(): int "
    "    var a: [3]int := [1, 2, 3]; "
    "    var b: [3]int := [1, 2, 3]; "
    "    var c: [3]int := [1, 2, 9]; "
    "    var d: [4]int := [1, 2, 3, 4]; "
    "    var oa: Object := a; var ob: Object := b; "
    "    var oc: Object := c; var od: Object := d; "
    "    var oi: Object := 7; "
    "    var code := 0; "
    "    if oa.Equals(ob) then code := code + 1; end; "
    "    if oa.Equals(oc) then code := code + 2; end; "
    "    if oa.Equals(od) then code := code + 4; end; "
    "    if oa.Equals(oi) then code := code + 8; end; "
    "    var ha: int := oa.GetHashCode(); "
    "    var hb: int := ob.GetHashCode(); "
    "    var hc: int := oc.GetHashCode(); "
    "    if ha = hb then code := code + 16; end; "
    "    if ha = hc then code := code + 32; end; "
    "    if ha = 0  then code := code + 64; end; "
    "    Console.WriteLine(code); "
    "    return 0; "
    "  end Main; "
    "end C;";

// §8.5: "the type's name — `\"[3]int\"`-style for an inline array".
const char* kToString3Int =
    "module M; class C "
    "  public static method Main(): int "
    "    var a: [3]int := [1, 2, 3]; "
    "    var o: Object := a; "
    "    Console.WriteLine(o.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// The same, at a non-primitive element and a different length — the name is
// derived from the type, not a fixed string.
const char* kToString2String =
    "module M; class C "
    "  public static method Main(): int "
    "    var a: [2]string := [\"x\", \"y\"]; "
    "    var o: Object := a; "
    "    Console.WriteLine(o.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// §8.6 / §15.1: the ELEMENT type is part of the identity too, and the box's
// Equals must reach each element kind's own equality (§15.1.2: "equal iff their
// elements are pairwise equal under the element type's equality") — string by
// content (§4.6), a record structurally (§9.1), a nullable value by its tag, a
// decimal by value (§4.4, not by scale bits). Bits 1..16 are the type tests,
// 32..256 the equalities, and bits 512/1024 are the discrimination guards that
// must stay clear. Want 511.
const char* kElementKinds =
    "module M; "
    "record Pt(X: int, Y: int); "
    "enum Col Red, Green; end Col; "
    "class C "
    "  public static method Main(): int "
    "    var s:  [2]string  := [\"ab\", \"cd\"]; "
    "    var s2: [2]string  := [\"ab\", \"cd\"]; "
    "    var p:  [2]Pt      := [new Pt(1, 2), new Pt(3, 4)]; "
    "    var p2: [2]Pt      := [new Pt(1, 2), new Pt(3, 4)]; "
    "    var n:  [2]int?    := [1, null]; "
    "    var n2: [2]int?    := [1, null]; "
    "    var e:  [2]Col     := [Col.Red, Col.Green]; "
    "    var d:  [2]decimal := [1.5m, 2.5m]; "
    "    var d2: [2]decimal := [1.5m, 2.5m]; "
    "    var os: Object := s; var os2: Object := s2; "
    "    var op: Object := p; var op2: Object := p2; "
    "    var on: Object := n; var on2: Object := n2; "
    "    var oe: Object := e; "
    "    var od: Object := d; var od2: Object := d2; "
    "    var code := 0; "
    "    if os is [2]string  then code := code + 1;   end; "
    "    if op is [2]Pt      then code := code + 2;   end; "
    "    if on is [2]int?    then code := code + 4;   end; "
    "    if oe is [2]Col     then code := code + 8;   end; "
    "    if od is [2]decimal then code := code + 16;  end; "
    "    if os.Equals(os2)   then code := code + 32;  end; "
    "    if op.Equals(op2)   then code := code + 64;  end; "
    "    if on.Equals(on2)   then code := code + 128; end; "
    "    if od.Equals(od2)   then code := code + 256; end; "
    "    if os is [2]Pt      then code := code + 512;  end; "
    "    if op is [2]string  then code := code + 1024; end; "
    "    Console.WriteLine(code); "
    "    return 0; "
    "  end Main; "
    "end C;";

// §8.6 "Unboxing reverses the process" — this held #2804's round trip: a
// MATCHING `o as [3]int` recovers the element values (456), where before the
// boxing fix it produced an all-zero array.
//
// RE-POINTED by #2911 (spec §17.4 "Degenerate value-type targets", v0.241),
// following the same discipline #2912 applied to kOwningNotBoxed above and
// #2804 itself applied to tests/lit/type_test_dropped_warns_2775.bzl. The
// spelling this test used — `var back := o as [3]int;` — is now a COMPILE
// ERROR: §15.1 admits no `([3]int)?` for the `T?` result §17.4 promises, so a
// FAILED cast could only ever manufacture an all-zero array indistinguishable
// from a legitimately zero one. That is precisely the gap the header above
// predicted and filed; the fix answers it by rejecting the operator.
//
// ⚠ THE 456 ASSERTION WAS NOT GONE BY CHOICE — IT WAS UNREACHABLE, AND THAT WAS
// TRACKED. §17.4's diagnostic names the replacement unbox: the §12.7 `is` test
// with narrowing and copy-out (`if o is [3]int then var b: [3]int := o; … end;`).
// MEASURED at #2911: that spelling compiled but yielded an ALL-ZERO array,
// because §12.7 narrowing to a value type was never unboxed by codegen — filed
// as **#3090** (an `int` there read the box POINTER, 866251552; a record threw).
// So there was then NO working spelling of "recover the values of a boxed inline
// array", and this test asserted the rejection instead.
//
// ✅ #3090 HAS LANDED (§12.7 / §8.6 / §17.4, v0.252) and the value assertion is
// RESTORED below as `kUnboxRoundTrip`, in the copy-out spelling — BOTH halves
// kept, exactly as the instruction required. #2804's round-trip intent is not
// deleted and is not relaxed to "compiles somehow": what stays pinned is that a
// boxed inline array's values survive the trip, which is the whole point of
// §8.6's "Unboxing reverses the process".
const char* kUnboxRoundTripNowRejected =
    "module M; class C "
    "  public static method Main(): int "
    "    var a: [3]int := [4, 5, 6]; "
    "    var o: Object := a; "
    "    var back := o as [3]int; "
    "    Console.WriteLine(back[0] * 100 + back[1] * 10 + back[2]); "
    "    return 0; "
    "  end Main; "
    "end C;";

// #2804's ROUND TRIP, RESTORED by #3090 (spec §12.7 / §8.6 / §17.4, v0.252) in
// the spelling §17.4's own diagnostic prints as the remedy for the `as` form it
// rejects, verbatim: "if o is [3]int then var a: [3]int := o; end;". §8.6's
// *Nullable value-type boxing* states the property this pins — "Unboxing
// reverses the process" — and §12.7 supplies the narrowing that admits the
// copy-out. Same 456, same intent, a legal spelling. Wants "456".
const char* kUnboxRoundTrip =
    "module M; class C "
    "  public static method Main(): int "
    "    var a: [3]int := [4, 5, 6]; "
    "    var o: Object := a; "
    "    if o is [3]int then "
    "      var back: [3]int := o; "
    "      Console.WriteLine(back[0] * 100 + back[1] * 10 + back[2]); "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// CONTROL. §8.6: "Move-only value types — owning inline arrays (`[N owned]T`
// and `[N owned]T?`) … — cannot be boxed." The guard is that #2804's fix did
// NOT start boxing them: boxing would copy a move-only value and hand two
// owners to the same instances.
//
// RE-POINTED by #2912 (spec §8.6/§15.3, v0.240), following the same discipline
// #2804 applied to tests/lit/type_test_dropped_warns_2775.bzl. When this test
// was written, "not boxed" was observable only as behaviour: the program
// compiled, the `is [2]R` test was DROPPED (codegen's #2775 `internal:`
// Warning), and it exited 0. #2912 then supplied the rejection §8.6's "cannot
// be boxed" always owed, so the same program no longer compiles — and the exit
// code 0 this once asserted is now unreachable. The ASSERTION being preserved
// is "#2804 did not sweep the owning form in with the non-owning one", and a
// compile REJECT proves that at least as strongly as a dropped test did, so the
// test is re-pointed at the rejection rather than deleted.
//
// ⚠ If a later change gives move-only values a boxing story, re-point this at
// whatever §8.6 then excludes — do NOT delete the assertion, and do NOT relax
// it to "compiles somehow": what must stay pinned is that a move-only value
// never silently acquires a heap copy.
const char* kOwningNotBoxed =
    "module M; "
    "class R public constructor() end constructor; end R; "
    "class C "
    "  public static method Main(): int "
    "    var own: [2 owned]R := [new R(), new R()]; "
    "    var oo: Object := own; "
    "    if oo is [2]R then return 1; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// §8.6 / §28.9 / #422: the new box is runtime-managed and refcounted. A loop
// that boxes an inline array into a non-escaping local each iteration must reach
// a steady state, and the exit code independently proves the boxing happened
// (without the fix `acc` stays 0 and the program returns 1).
const char* kBoxLoopSteadyState =
    "module M; class C "
    "  public static method Main(): int "
    "    var i := 0; var acc := 0; "
    "    while i < 20000 do "
    "      var a: [3]int := [i, i + 1, i + 2]; "
    "      var o: Object := a; "
    "      if o is [3]int then acc := acc + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    if acc = 20000 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// §8.6 / §28.9 — the box-vs-class hazard with inline-array boxes in the mix: a
// class instance in an Object slot is NOT a box and must be disposed exactly
// once by ownership, while the `[3]int` box, the `[2]string` box (whose payload
// holds runtime-managed string references) and the int box are each released
// exactly once. A double-free / UAF aborts under ASan.
// acc = 5000 * 4 = 20000.
const char* kMixedBoxAndClassLoop =
    "module M; class C "
    "  public static method Main(): int "
    "    var i := 0; var acc := 0; "
    "    while i < 5000 do "
    "      var a: [3]int := [1, 2, 3]; "
    "      var oa: Object := a; "
    "      var s: [2]string := [\"x\", \"y\"]; "
    "      var os: Object := s; "
    "      var oi: Object := i; "
    "      using var inst := new C(); "
    "      var oc: Object := inst; "
    "      acc := acc + (if oa is [3]int then 1 else 0) "
    "                 + (if os is [2]string then 1 else 0) "
    "                 + (if oi is int then 1 else 0) "
    "                 + (if oc is C then 1 else 0); "
    "      i := i + 1; "
    "    end; "
    "    if acc = 20000 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// OVER-REJECTION SWEEP (and, measured, a regression test too: 255 pre-fix). The
// gate `isBoxableValueType` and the descriptor resolver `boxDescriptorFor` are
// shared by EVERY boxable kind, so this asserts that none of the kinds that
// already boxed changed: each still boxes and matches itself (bit 1); none of them
// matches an inline-array target (bit 2); the inline-array box matches none of
// THEIR targets (bit 4); ToString / Equals / GetHashCode through an Object
// receiver still behave (bits 8/16/32); IComparable / IEquatable dispatch on a
// boxed primitive is untouched (bits 64/128); and the bz_obj_is_string
// discriminator still keeps a bare bz_string* in an Object slot away from the
// descriptor walk when the target is an inline array (bit 256). Want 511.
const char* kOtherBoxableKindsUnchanged =
    "module M; "
    "record Pt(X: int, Y: int); "
    "enum Col Red, Green; end Col; "
    "class K public constructor() end constructor; end K; "
    "class C "
    "  public static method Main(): int "
    "    var code := 0; "
    "    var oi: Object := 7; "
    "    var ol: Object := 9000000000L; "
    "    var od: Object := 1.5; "
    "    var ob: Object := true; "
    "    var oc: Object := 'x'; "
    "    var om: Object := 12.5m; "
    "    var os: Object := \"hi\"; "
    "    var orec: Object := new Pt(1, 2); "
    "    var oe: Object := Col.Red; "
    "    using var k := new K(); "
    "    var ok: Object := k; "
    "    var arr: [3]int := [1, 2, 3]; "
    "    var oarr: Object := arr; "
    "    if oi is int and ol is long and od is double and ob is bool "
    "       and oc is char and om is decimal and os is string "
    "       and orec is Pt and oe is Col and ok is K "
    "      then code := code + 1; end; "
    "    if not (oi is [3]int) and not (ol is [3]int) and not (od is [3]int) "
    "       and not (ob is [3]int) and not (oc is [3]int) "
    "       and not (om is [3]int) and not (os is [3]int) "
    "       and not (orec is [3]int) and not (oe is [3]int) "
    "       and not (ok is [3]int) "
    "      then code := code + 2; end; "
    "    if not (oarr is int) and not (oarr is long) and not (oarr is double) "
    "       and not (oarr is bool) and not (oarr is char) "
    "       and not (oarr is decimal) and not (oarr is string) "
    "       and not (oarr is Pt) and not (oarr is Col) and not (oarr is K) "
    "      then code := code + 4; end; "
    "    var oi2: Object := 7; var oi3: Object := 8; "
    "    if oi.ToString() = \"7\" and od.ToString() = \"1.5\" "
    "       and ob.ToString() = \"True\" then code := code + 8; end; "
    "    if oi.Equals(oi2) and not oi.Equals(oi3) then code := code + 16; end; "
    "    if oi.GetHashCode() = oi2.GetHashCode() "
    "       and oi.GetHashCode() <> oi3.GetHashCode() then code := code + 32; end; "
    "    var ci: IComparable<int> := 3; "
    "    if ci.CompareTo(9) = -1 and ci.CompareTo(1) = 1 and ci.CompareTo(3) = 0 "
    "      then code := code + 64; end; "
    "    var ei: IEquatable<int> := 3; "
    "    if ei.Equals(3) and not ei.Equals(4) then code := code + 128; end; "
    "    if oarr is Object and not (os is [3]int) then code := code + 256; end; "
    "    Console.WriteLine(code); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #2804 — the cardinal repro: `o is [3]int` on an Object-typed inline array.
// =====================================================================

BEZEL_TEST(codegen_box_inline_array_2804,
           object_typed_inline_array_matches_only_its_own_type_spec_8_6) {
    // spec §8.6, v0.240: "Non-owning inline arrays (`[N]T` and `[N]T?`) are
    // ordinary value types and box per the general rule." A box carries a
    // precise runtime tag, so exactly `is [3]int` and `is Object` may match.
    // Without the fix the Object local held the raw `[3 x i32]` aggregate, every
    // test (including `is Object`, whose operand must be a pointer) was dropped,
    // and the program printed 0.
    BEZEL_EXPECT_EQ(runAotOut(kTypeTest), std::string("65"));
}

// =====================================================================
// #2804 — §8.3 Equals / GetHashCode through an Object receiver.
// =====================================================================

BEZEL_TEST(codegen_box_inline_array_2804,
           boxed_inline_array_equals_and_hash_are_elementwise_spec_8_3) {
    // spec §8.3, v0.240, inline-array row: "Element-by-element value equality
    // ([§15.1.2])" / "Hash combining element hashes in slot order", tied by "if
    // `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`". A different
    // element value, a different LENGTH and a boxed int are all unequal, and the
    // hash is neither degenerate nor length/value-blind. Want 1 + 16 = 17.
    BEZEL_EXPECT_EQ(runAotOut(kEqualsAndHash), std::string("17"));
}

// =====================================================================
// #2804 — §8.5 ToString through an Object receiver.
// =====================================================================

BEZEL_TEST(codegen_box_inline_array_2804,
           boxed_inline_array_tostring_is_the_type_name_spec_8_5) {
    // spec §8.5, v0.240: "the type's name — `\"[3]int\"`-style for an inline
    // array". Not an element listing — §8.5 says that is deliberately not
    // synthesized.
    BEZEL_EXPECT_EQ(runAotOut(kToString3Int), std::string("[3]int"));
}

BEZEL_TEST(codegen_box_inline_array_2804,
           boxed_inline_array_tostring_tracks_the_element_type_spec_8_5) {
    BEZEL_EXPECT_EQ(runAotOut(kToString2String), std::string("[2]string"));
}

// =====================================================================
// #2804 — the element type is part of the identity, and of the equality.
// =====================================================================

BEZEL_TEST(codegen_box_inline_array_2804,
           boxed_inline_array_element_kinds_spec_8_6_8_3) {
    // §15.1.2: two inline arrays "are equal iff their elements are pairwise
    // equal under the element type's equality" — so string elements compare by
    // content, record elements structurally, nullable elements by their tag, and
    // decimal elements by value. §15.1 makes `[2]string` and `[2]Pt` distinct
    // types, which the last two bits guard. Want 511.
    BEZEL_EXPECT_EQ(runAotOut(kElementKinds), std::string("511"));
}

// =====================================================================
// #2804 — §8.6 "Unboxing reverses the process".
// =====================================================================

// #2804 / #3090 — the round trip itself, in the spelling §17.4 sanctions. This
// is the assertion #2911 parked; it is back, and the rejection test below keeps
// its own half. Pre-#3090 this printed nothing at all (the copy-out produced an
// all-zero array and drew codegen's #2434 note); want "456".
BEZEL_TEST(codegen_box_inline_array_2804,
           boxed_inline_array_is_narrowing_copy_out_round_trip_spec_8_6_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(kUnboxRoundTrip), std::string("456"));
}

BEZEL_TEST(codegen_box_inline_array_2804,
           boxed_inline_array_as_unbox_now_rejected_spec_17_4_15_1) {
    // This test used to read
    //   BEZEL_EXPECT_EQ(runAotOut(kUnboxRoundTrip), std::string("456"));
    // and that assertion is RESTORED — in the §12.7 copy-out spelling — by the
    // sibling test immediately above, now that #3090 has landed. The `as`
    // spelling stays rejected; both halves are pinned.
    //
    // §17.4 (v0.241) now rejects the `as` spelling outright: `as` "requires its
    // result type `T?` to exist and to be informative", and §15.1 "admits no
    // nullable inline-array form", so the operator's contract is unsatisfiable
    // — a failed cast had nowhere to put the `null` and manufactured an
    // all-zero array instead (#2911).
    JitResult r = runAot(kUnboxRoundTripNowRejected);
    BEZEL_EXPECT_FALSE(r.ran);
    bool rejected = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("admits no nullable inline-array form") !=
                std::string::npos &&
            d.message.find("§17.4") != std::string::npos)
            rejected = true;
    BEZEL_EXPECT_TRUE(rejected);
}

// =====================================================================
// #2804 — CONTROL: §8.6's move-only exclusion still holds.
// =====================================================================

BEZEL_TEST(codegen_box_inline_array_2804,
           owning_inline_array_is_still_not_boxed_spec_8_6) {
    // spec §8.6, v0.240: "Move-only value types — owning inline arrays
    // (`[N owned]T` and `[N owned]T?`) and owning-classified choices — cannot be
    // boxed. Boxing a value type copies the value into a heap wrapper, but
    // move-only types forbid copying ([§15.3])." The guard is that #2804's new
    // boxing path did not sweep the owning form in with the non-owning one,
    // which would copy a move-only value and give two owners to the same
    // instances.
    //
    // RE-POINTED by #2912 (see kOwningNotBoxed's note): the rejection §8.6 owes
    // now exists, so this asserts the REJECT rather than the exit code of a
    // program that used to compile with its type test silently dropped.
    JitResult r = runAot(kOwningNotBoxed);
    BEZEL_EXPECT_FALSE(r.ran);
    bool rejected = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("move-only value type") != std::string::npos &&
            d.message.find("cannot be boxed") != std::string::npos &&
            d.message.find("§8.6") != std::string::npos)
            rejected = true;
    BEZEL_EXPECT_TRUE(rejected);
}

// =====================================================================
// #2804 — §28.9 / #422: the new box is refcounted and memory-clean.
// =====================================================================

BEZEL_TEST(codegen_box_inline_array_2804,
           inline_array_box_loop_reaches_steady_state_spec_28_9) {
    // The payload is an aggregate whose size varies with the type, so this box
    // is the first whose allocation size is not a compile-time constant of the
    // box machinery; a leak would be new with this change. runAotChecked's LSan
    // leg flips r.ran on a leak / double-free / use-after-free, and the exit code
    // independently proves the boxing happened (pre-fix `acc` stays 0 → 1).
    JitResult r = runAotChecked(kBoxLoopSteadyState);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(codegen_box_inline_array_2804,
           inline_array_box_and_class_instance_mixed_loop_clean_spec_28_9) {
    // The box-vs-class hazard: the class instance in an Object slot is disposed
    // exactly once by ownership and must never be touched by box-release, while
    // the `[3]int`, `[2]string` and int boxes are each released exactly once.
    // The `[2]string` payload holds runtime-managed string references, so a
    // box-release that walked the payload would show up here.
    JitResult r = runAotChecked(kMixedBoxAndClassLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// #2804 — CONTROL: over-rejection sweep over every kind that already boxed.
// =====================================================================

BEZEL_TEST(codegen_box_inline_array_2804,
           other_boxable_kinds_are_unchanged_spec_8_6_8_3) {
    // Primarily the false-reject / false-accept guard on the shared surface
    // (isBoxableValueType, boxDescriptorFor, boxVtableFor) — but not vacuous:
    // measured at 255 without the fix, because bit 256 needs the box to exist.
    // int / long / double / bool / char / decimal / string / record / enum /
    // class instance all still box, still match themselves, and still keep their
    // ToString / Equals / GetHashCode / IComparable / IEquatable behavior; none
    // of them matches an inline-array target and the inline-array box matches
    // none of theirs. Want 511.
    BEZEL_EXPECT_EQ(runAotOut(kOtherBoxableKindsUnchanged), std::string("511"));
}
