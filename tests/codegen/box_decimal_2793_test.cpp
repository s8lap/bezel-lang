// Codegen — §8.6 / #2793: a `decimal` used where `Object` (or an interface it
// implements) is expected must be BOXED, like every other non-move-only value
// type. It was not, so every type test on it silently answered `false`.
//
// spec §8.6 (v0.240), the general rule:
//   "When a value type is used where `Object` (or an interface implemented by
//    the value type) is expected, the value is **boxed**: a heap-allocated
//    wrapper holds the value."
//
// and §8.6's ONLY stated exception, which decimal is not in:
//   "Move-only value types — owning inline arrays (`[N owned]T` and
//    `[N owned]T?`) and owning-classified choices ([§35.4]) — cannot be boxed."
//
// spec §8.3 (v0.240) fixes what the box's Equals/GetHashCode must mean — the
// type-kind table's Primitive row reads `Value comparison` / `Value-based`, and
// the section states the tying invariant:
//   "`GetHashCode` must satisfy: if `a.Equals(b)` then
//    `a.GetHashCode() = b.GetHashCode()`."
//
// stdlib §9.3 (v0.240) declares `public record Decimal: IComparable<decimal>,
// IEquatable<decimal>`, so a boxed decimal must also dispatch those two members
// through the box itable.
//
// THE DEFECT. Three coordinated sites excluded `decimal` from boxing:
// `primBoxName` returned null for it (no `ptd$decimal` descriptor, no
// `pvt$decimal` box vtable), `setupBoxDescriptors`' hand-written `prims[]` array
// omitted it, and `isBoxableValueType` excluded `P::Decimal` explicitly —
// against its own comment, which said "everything else (numeric, bool, char) is
// a value primitive that boxes". With `boxVtableFor` returning null, `boxValue`
// returned the value UNCHANGED: the `Object` local held a raw 16-byte
// `%bz.decimal` struct rather than a box pointer, and every `is`/`as` on it was
// folded to a constant `false` / `null`. Silent wrong value, no diagnostic.
//
// THE FIX. One box-primitive table (`BZ_BOX_PRIMITIVES`) now feeds all three
// sites, with `decimal` in it; the per-primitive Equals/CompareTo thunks, which
// were written for SCALAR payloads, gained a struct-payload path that routes
// through `bz_decimal_cmp` — the same arbiter `=`, record-field equality and
// `bz_decimal_hash` already use — rather than an `ICmpEQ` on a struct (which is
// invalid IR, and would compare scale bits rather than values).
//
// WHAT EACH TEST SHOWS WITHOUT THE FIX (measured by reverting the src change
// and re-running: 9 of the 10 go red):
//   type_test:   0  instead of 1    (no test matched — the issue's own repro)
//   equals_hash: 24 instead of 9    (Equals false; both hashes the degenerate 0)
//   interface:   4  instead of 15   (both itable calls degraded to zero values)
//   tostring:    ""  instead of "12.5"
//   unbox:       "NULL" instead of "12.5"
//   leak loop:   1  instead of 0    (acc never incremented)
//   mixed loop:  1  instead of 0
//   sweep:       does not COMPILE — a dropped `is decimal` in a non-first
//                position of a short-circuit chain emits a basic block with no
//                terminator, so bezelc reports "IR verification failed"
// so none of them is vacuous. The one test that passes both ways —
// unbox_of_a_non_decimal_box_is_null — is deliberately a CONTROL: it is the
// guard that the fix did not make the decimal box match everything.
//
// NOT COVERED HERE, deliberately:
//   * The §4.4 scale-equality property (12.5m vs 12.50m through the box) is not
//     asserted because it is NOT OBSERVABLE in this implementation: every
//     decimal producer routes through the runtime's dec_store, whose first act
//     is "Remove trailing decimal zeros to minimise scale", so two equal
//     decimals always have identical bytes and a byte compare would pass such a
//     test for the wrong reason. Routing box Equals through bz_decimal_cmp is
//     still required — an ICmpEQ on the {i32×4} payload is invalid IR — and it
//     keeps the property true by construction if a non-canonical decimal ever
//     appears (e.g. across the FFI boundary).
//   * Cross-module box identity: a decimal box minted in a library and tested in
//     an importing artifact answers `false` — but so does an `int` box, in the
//     identical shape (measured both directions with a two-module probe). That
//     is #2301, not decimal-specific and not made worse here.
//   * Non-owning inline arrays, §8.6's other unboxed value kind: #2804.
//   * A dropped type test in a short-circuit chain: #2805.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

// §8.6: the issue's cardinal repro, widened to every neighbouring primitive so
// the fix cannot pass by making the box match EVERYTHING. Exactly one bit must
// be set. Without the fix nothing matches and the program returns 0.
const char* kDecimalTypeTest =
    "module M; class C "
    "  public static method Main(): int "
    "    var o: Object := 12.5m; "
    "    var code := 0; "
    "    if o is decimal then code := code + 1; end; "
    "    if o is double  then code := code + 2; end; "
    "    if o is int     then code := code + 4; end; "
    "    if o is string  then code := code + 8; end; "
    "    if o is long    then code := code + 16; end; "
    "    if o is float   then code := code + 32; end; "
    "    return code; "
    "  end Main; "
    "end C;";

// §8.6 "Unboxing reverses the process": the boxed decimal round-trips through
// `as decimal` with its value intact, and a NON-matching unbox yields null.
const char* kDecimalUnboxRoundTrip =
    "module M; class C "
    "  public static method Main(): int "
    "    var o: Object := 12.5m; "
    "    var back: decimal? := o as decimal; "
    "    if back <> null then "
    "      var v: decimal := back!; "
    "      Console.WriteLine(v); "
    "    else "
    "      Console.WriteLine(\"NULL\"); "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kDecimalUnboxMiss =
    "module M; class C "
    "  public static method Main(): int "
    "    var o: Object := 7; "
    "    var back: decimal? := o as decimal; "
    "    if back <> null then return 1; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// §8.3 (Primitive → "Value comparison" / "Value-based") through the box: two
// SEPARATE boxes of the same value are Equals and hash alike; a different value
// is not; a box of a different primitive is not. The `ha = 0` probe is the
// non-vacuity guard — without the fix GetHashCode on the unboxed bits returned
// the degenerate 0 for every decimal, which would satisfy `ha = hb` for free.
// Want: 1 (a=b) + 8 (hash agrees) = 9.
const char* kDecimalEqualsAndHash =
    "module M; class C "
    "  public static method Main(): int "
    "    var a: Object := 12.5m; "
    "    var b: Object := 12.5m; "
    "    var c: Object := 12.6m; "
    "    var i: Object := 12; "
    "    var code := 0; "
    "    if a.Equals(b) then code := code + 1; end; "
    "    if a.Equals(c) then code := code + 2; end; "
    "    if a.Equals(i) then code := code + 4; end; "
    "    var ha: int := a.GetHashCode(); "
    "    var hb: int := b.GetHashCode(); "
    "    if ha = hb then code := code + 8; end; "
    "    if ha = 0  then code := code + 16; end; "
    "    return code; "
    "  end Main; "
    "end C;";

// §8.5 ToString through an Object receiver — the boxed form must agree with the
// direct (unboxed) form, which is bz_decimal_to_string.
const char* kDecimalBoxToString =
    "module M; class C "
    "  public static method Main(): int "
    "    var o: Object := 12.5m; "
    "    var s: string := o.ToString(); "
    "    Console.WriteLine(s); "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kDecimalBoxToStringNegativeFraction =
    "module M; class C "
    "  public static method Main(): int "
    "    var o: Object := -0.001m; "
    "    var s: string := o.ToString(); "
    "    Console.WriteLine(s); "
    "    return 0; "
    "  end Main; "
    "end C;";

// stdlib §9.3 `Decimal: IComparable<decimal>, IEquatable<decimal>` — the box
// itable must carry real CompareTo/Equals thunks. The struct payload has no
// ICmp/FCmp form, so these are the thunks that had to grow a decimal path.
// Want 1 + 2 + 4 + 8 = 15.
const char* kDecimalInterfaceDispatch =
    "module M; class C "
    "  public static method Main(): int "
    "    var ic: IComparable<decimal> := 12.5m; "
    "    var r1: int := ic.CompareTo(20.0m); "
    "    var r2: int := ic.CompareTo(1.0m); "
    "    var r3: int := ic.CompareTo(12.5m); "
    "    var ie: IEquatable<decimal> := 12.5m; "
    "    var e1: bool := ie.Equals(12.5m); "
    "    var e2: bool := ie.Equals(12.6m); "
    "    var code := 0; "
    "    if r1 = -1 then code := code + 1; end; "
    "    if r2 = 1  then code := code + 2; end; "
    "    if r3 = 0  then code := code + 4; end; "
    "    if e1 then code := code + 8; end; "
    "    if e2 then code := code + 16; end; "
    "    return code; "
    "  end Main; "
    "end C;";

// §8.6 / §28.9: the box is runtime-managed and refcounted (#422). A loop that
// boxes a decimal into a non-escaping local each iteration must reach a steady
// state — no leak — and the exit code proves the boxing actually happened
// (without the fix `acc` stays 0 and the program returns 1).
const char* kDecimalBoxLoopSteadyState =
    "module M; class C "
    "  public static method Main(): int "
    "    var i := 0; var acc := 0; "
    "    while i < 20000 do "
    "      var o: Object := 12.5m; "
    "      if o is decimal then acc := acc + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    if acc = 20000 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// §8.6 / §28.9 — the box-vs-class hazard, with a decimal box in the mix: a
// class instance in an `Object` slot is NOT a box and must be disposed exactly
// once by ownership; the decimal box, the int box and the interface-typed
// decimal box must each be released exactly once. A double-free / UAF aborts
// under ASan. acc = 5000 * (1 + 1 + 1 + 1) = 20000.
const char* kDecimalBoxAndClassMixedLoop =
    "module M; class C "
    "  public static method Main(): int "
    "    var i := 0; var acc := 0; "
    "    while i < 5000 do "
    "      var od: Object := 12.5m; "
    "      var oi: Object := i; "
    "      using var inst := new C(); "
    "      var oc: Object := inst; "
    "      var ic: IComparable<decimal> := 0.5m; "
    "      var r: int := ic.CompareTo(0.25m); "
    "      acc := acc + (if od is decimal then 1 else 0) "
    "                 + (if oi is int then 1 else 0) "
    "                 + (if oc is C then 1 else 0) "
    "                 + r; "
    "      i := i + 1; "
    "    end; "
    "    if acc = 20000 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// OVER-REJECTION / REGRESSION SWEEP. The four per-primitive thunk builders and
// the box-primitive table are SHARED by every primitive, so the sweep asserts
// that none of the already-working ones changed: each still boxes, still tests
// as itself, still does NOT test as decimal, and its ToString / Equals /
// GetHashCode / IComparable / IEquatable still behave. Want all 12 bits: 4095 —
// which does NOT fit an 8-bit exit status, so the value goes to stdout and the
// test reads it with runAotOut.
const char* kOtherPrimitivesUnchanged =
    "module M; class C "
    "  public static method Main(): int "
    "    var by: byte := 200; var sb: sbyte := -5; var sh: short := -300; "
    "    var us: ushort := 900; var ui: uint := 4000000000; "
    "    var ul: ulong := 18000000000000000000; var fl: float := 2.5f; "
    "    var oi: Object := 7;    var ol: Object := 9000000000L; "
    "    var od: Object := 1.5;  var ob: Object := true; "
    "    var oc: Object := 'x';  var oy: Object := by; "
    "    var os: Object := sb;   var oh: Object := sh; "
    "    var ou: Object := us;   var ov: Object := ui; "
    "    var ow: Object := ul;   var of: Object := fl; "
    "    var code := 0; "
    // every primitive still matches ITSELF
    "    if oi is int and ol is long and od is double and ob is bool "
    "       and oc is char and oy is byte and os is sbyte and oh is short "
    "       and ou is ushort and ov is uint and ow is ulong and of is float "
    "      then code := code + 1; end; "
    // and NONE of them matches decimal
    "    if not (oi is decimal) and not (ol is decimal) "
    "       and not (od is decimal) and not (ob is decimal) "
    "       and not (oc is decimal) and not (oy is decimal) "
    "       and not (os is decimal) and not (oh is decimal) "
    "       and not (ou is decimal) and not (ov is decimal) "
    "       and not (ow is decimal) and not (of is decimal) "
    "      then code := code + 2; end; "
    // a string in an Object slot is NOT boxed and still tests as string
    "    var ostr: Object := \"hi\"; "
    "    if ostr is string and not (ostr is decimal) then code := code + 4; end; "
    // ToString through the box
    "    var s1: string := oi.ToString(); var s2: string := od.ToString(); "
    "    var s3: string := ob.ToString(); var s4: string := oc.ToString(); "
    "    if s1 = \"7\" and s2 = \"1.5\" and s3 = \"True\" and s4 = \"x\" "
    "      then code := code + 8; end; "
    // Equals + GetHashCode through the box (§8.3)
    "    var oi2: Object := 7; var oi3: Object := 8; var od2: Object := 1.5; "
    "    if oi.Equals(oi2) and not oi.Equals(oi3) and od.Equals(od2) "
    "      then code := code + 16; end; "
    "    var h1: int := oi.GetHashCode(); var h2: int := oi2.GetHashCode(); "
    "    var h3: int := oi3.GetHashCode(); "
    "    if h1 = h2 and h1 <> h3 then code := code + 32; end; "
    // IComparable/IEquatable dispatch: signed, unsigned and FP orderings
    "    var ci: IComparable<int> := 3; "
    "    if ci.CompareTo(9) = -1 and ci.CompareTo(1) = 1 and ci.CompareTo(3) = 0 "
    "      then code := code + 64; end; "
    "    var cd: IComparable<double> := 9.5; "
    "    if cd.CompareTo(1.5) = 1 and cd.CompareTo(20.5) = -1 "
    "      then code := code + 128; end; "
    "    var cu: IComparable<uint> := ui; "
    "    var one_ui: uint := 1; "
    "    if cu.CompareTo(one_ui) = 1 then code := code + 256; end; "
    "    var one_ul: ulong := 1; "
    "    var cl: IComparable<ulong> := one_ul; "
    "    if cl.CompareTo(ul) = -1 then code := code + 512; end; "
    "    var ei: IEquatable<int> := 3; "
    "    if ei.Equals(3) and not ei.Equals(4) then code := code + 1024; end; "
    "    var ed: IEquatable<double> := 9.5; "
    "    if ed.Equals(9.5) and not ed.Equals(1.5) then code := code + 2048; end; "
    "    Console.WriteLine(code); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #2793 — the cardinal repro: `o is decimal` on an Object-typed decimal.
// =====================================================================

BEZEL_TEST(codegen_box_decimal_2793,
           object_typed_decimal_matches_only_decimal_spec_8_6) {
    // spec §8.6, v0.240: "When a value type is used where `Object` ... is
    // expected, the value is **boxed**". decimal is not in §8.6's move-only
    // exception, so it boxes and `o is decimal` must be true — and, because the
    // box carries a precise `ptd$decimal` type tag, ONLY that test may match.
    // Without the fix the Object local held raw `%bz.decimal` bits, no test
    // matched, and the program returned 0.
    JitResult r = runAot(kDecimalTypeTest);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// =====================================================================
// #2793 — §8.6 "Unboxing reverses the process".
// =====================================================================

BEZEL_TEST(codegen_box_decimal_2793,
           boxed_decimal_unboxes_with_value_intact_spec_8_6) {
    // The value must survive the round trip, not merely the type test.
    BEZEL_EXPECT_EQ(runAotOut(kDecimalUnboxRoundTrip), std::string("12.5"));
}

BEZEL_TEST(codegen_box_decimal_2793,
           unbox_of_a_non_decimal_box_is_null_spec_8_6) {
    // The complement: `as decimal` on a boxed int yields null, so the fix did
    // not simply make the box match everything.
    JitResult r = runAot(kDecimalUnboxMiss);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// #2793 — §8.3 Equals / GetHashCode through an Object receiver.
// =====================================================================

BEZEL_TEST(codegen_box_decimal_2793,
           boxed_decimal_equals_and_hash_are_value_based_spec_8_3) {
    // spec §8.3, v0.240: the type-kind table gives a Primitive `Value
    // comparison` Equals and a `Value-based` GetHashCode, and "`GetHashCode`
    // must satisfy: if `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`".
    // Two separate boxes of 12.5m are equal and hash alike; 12.6m is not equal;
    // a boxed int is not equal. `ha = 0` must NOT hold — that is the guard
    // against the pre-fix degenerate hash, under which `ha = hb` was true for
    // the wrong reason. Want 1 + 8 = 9 (pre-fix: 8 + 16 = 24).
    JitResult r = runAot(kDecimalEqualsAndHash);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}

// =====================================================================
// #2793 — §8.5 ToString through an Object receiver.
// =====================================================================

BEZEL_TEST(codegen_box_decimal_2793,
           boxed_decimal_tostring_matches_direct_form_spec_8_5) {
    BEZEL_EXPECT_EQ(runAotOut(kDecimalBoxToString), std::string("12.5"));
}

BEZEL_TEST(codegen_box_decimal_2793,
           boxed_decimal_tostring_negative_fraction_spec_8_5) {
    BEZEL_EXPECT_EQ(runAotOut(kDecimalBoxToStringNegativeFraction),
                    std::string("-0.001"));
}

// =====================================================================
// #2793 — stdlib §9.3 IComparable<decimal> / IEquatable<decimal> dispatch.
// =====================================================================

BEZEL_TEST(codegen_box_decimal_2793,
           boxed_decimal_interface_dispatch_stdlib_9_3) {
    // stdlib §9.3, v0.240: `public record Decimal: IComparable<decimal>,
    // IEquatable<decimal>`. The itable thunks take the payload BY VALUE and had
    // no struct form, so before the fix both calls degraded to zero-initialized
    // results (r1 = r2 = r3 = 0, e1 = e2 = false → 4). Routing through
    // bz_decimal_cmp gives the real -1 / 1 / 0 and true / false → 15.
    JitResult r = runAot(kDecimalInterfaceDispatch);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 15);
}

// =====================================================================
// #2793 — §28.9 / #422: the new box is refcounted and memory-clean.
// =====================================================================

BEZEL_TEST(codegen_box_decimal_2793,
           decimal_box_loop_reaches_steady_state_spec_28_9) {
    // The 16-byte `%bz.decimal` payload makes this box 32 bytes rather than the
    // 24 every other primitive box takes; a leak would therefore be new with
    // this change. runAotChecked's LSan leg flips r.ran on a leak / double-free
    // / use-after-free; the exit code independently proves the boxing happened
    // (pre-fix `acc` stays 0 and the program returns 1).
    JitResult r = runAotChecked(kDecimalBoxLoopSteadyState);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(codegen_box_decimal_2793,
           decimal_box_and_class_instance_mixed_loop_clean_spec_28_9) {
    // The box-vs-class hazard with a decimal box in the mix: the class instance
    // in an Object slot is disposed exactly once by ownership and must never be
    // touched by box-release, while the decimal / int / interface-typed decimal
    // boxes are each released exactly once.
    JitResult r = runAotChecked(kDecimalBoxAndClassMixedLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// #2793 — over-rejection sweep: nothing else that already boxed changed.
// =====================================================================

BEZEL_TEST(codegen_box_decimal_2793,
           other_primitive_boxes_are_unchanged_spec_8_6_8_3) {
    // The shared surface: the box-primitive table, primBoxToStringThunk,
    // primBoxEqualsThunk, primBoxHashThunk and buildPrimBoxItmap are used by
    // EVERY primitive. All twelve still box, test as themselves, do NOT test as
    // decimal, and keep their ToString / Equals / GetHashCode / IComparable /
    // IEquatable behavior — including the signed, UNSIGNED and floating-point
    // orderings, which the itmap's signedness classification distinguishes.
    // 4095 does not fit an 8-bit exit status, so it travels on stdout.
    BEZEL_EXPECT_EQ(runAotOut(kOtherPrimitivesUnchanged), std::string("4095"));
}
