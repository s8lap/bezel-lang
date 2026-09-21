// Codegen — issue #4562: `decimal.Equals(...)` silently returned `false` for
// EVERY argument, including a value-equal `decimal` — no crash, no
// diagnostic, no IR-verifier complaint. `bezelc` compiled the program clean
// and it ran to completion answering the wrong result.
//
// MECHANISM. `tryLowerValueSemanticsCall` (LlvmBackend_Calls2.cpp) correctly
// classifies a `decimal` receiver as `primValueSem` and dispatches to
// `lowerValueSemMethodOn`. That function's primitive-receiver `Equals`/
// `GetHashCode` arm tested only `recv->getType()->isFloatingPointTy()` and
// `isIntegerTy()` — but `decimal` lowers to a 16-byte STRUCT
// (`decimalStructType()`, `{i32, i32, i32, i32}`), so neither branch matched
// and the arm fell through to an unconditional `return nullptr;`. The
// caller's own fallback then answered a silent `false` with no diagnostic.
// `GetHashCode` had the identical gap (`isDoubleTy`/`isFloatingPointTy`/
// `isIntegerTy`, all false for a struct payload).
//
// THE FIX adds a decimal-struct branch to both members of that arm, routed
// through EXISTING machinery rather than a new dispatch site:
//   - `GetHashCode`: `emitValueHashCode(recv, recvTy)` — the same helper the
//     box-Equals thunk (`primBoxHashThunk`) and every decimal record-field/
//     dictionary-key hash already call, which itself calls `bz_decimal_hash`.
//   - `Equals`: classifies the ARGUMENT's static type before lowering it
//     (§22.5). An argument whose static type is decimal itself is compared
//     via `emitValueEquality(recv, other, recvTy)` — the SAME per-value
//     helper `=`, record-field equality, and the box-Equals thunk
//     (`primBoxEqualsThunk`, #2793) all call, which routes decimal through
//     `bz_decimal_cmp` so `1.5m` and `1.50m` compare equal despite differing
//     scale bits. Any other static shape (`Object`, a boxed different
//     primitive, …) is routed through `lowerValueSemUniversalEquals` — the
//     same box+dispatch+release helper #4559's sibling fix wired the int/FP
//     family's mismatched-shape argument to, and every record/enum/choice
//     receiver already uses.
//
// SCOPE. `decimal.Equals(...)` is independent of #4559 (`value.Equals
// (Object)` boxing for the int/FP family): decimal never reached that fix's
// `isInt`/`isFP` classification in LlvmBackend_Exprs.cpp at all (decimal is
// neither), so its plain-`.` `Equals`/`GetHashCode` spelling always fell
// through that file's whole primitive dispatch block unhandled and reached
// `lowerValueSemMethodOn` directly — the site THIS fix touches.
//
// FAIL-WITHOUT proved mechanically: with LlvmBackend_Calls2.cpp restored to
// this worktree's pre-fix HEAD (18ac1d5a) and the test binary rebuilt, the
// `decimal_equals_true_same_value_spec_8_3` and
// `decimal_equals_true_different_scale_spec_4_4` rows below both report `0`
// (false) against an expected `1` (true) — the issue's own repro; with the
// fix, all rows below pass.
//
// §4.4 / §8.3 (also §8.1 / §8.6 / §22.5 for the mismatched-argument-shape
// rows and §9.1 for GetHashCode coherence), v0.306.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── the issue's own repro ───────────────────────────────────────────────────

// §8.3 "Primitive | Value comparison": two decimals of the identical value
// must compare Equal. This is the row the pre-fix compiler answered `0`
// (false) for — the wrong-value oracle, not merely "compiles and runs".
BEZEL_TEST(decimal_equals_4562, decimal_equals_true_same_value_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 5.5m; "
        "  var e: decimal := 5.5m; "
        "  if d.Equals(e) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The UNEQUAL-pair row a fix that returned `true` unconditionally would not
// survive (CLAUDE.md: a wrong-value bug needs an unequal-pair control).
BEZEL_TEST(decimal_equals_4562, decimal_equals_false_different_value_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 5.5m; "
        "  var e: decimal := 9.25m; "
        "  if d.Equals(e) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §4.4 "Exact representation of finite decimal fractions": `5.5m` and
// `5.50m` are the SAME value under different scale bits. A byte/bit compare
// of the raw struct would wrongly answer False here; only the runtime
// `bz_decimal_cmp` normalizes scale before comparing.
BEZEL_TEST(decimal_equals_4562, decimal_equals_true_different_scale_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 5.5m; "
        "  var e: decimal := 5.50m; "
        "  if d.Equals(e) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── the `Object`-typed argument face (§8.1's universal `Equals`) ──────────

// §8.1/§8.6: `d.Equals(o)` where `o: Object` holds an EQUAL decimal value —
// the receiver boxes, the argument is already boxed, and the two boxes'
// `bz$primeq$decimal` thunk compares payload via `bz_decimal_cmp` (#2793).
BEZEL_TEST(decimal_equals_4562, decimal_equals_object_arg_true_spec_8_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5.5m; "
        "  var d: decimal := 5.5m; "
        "  if d.Equals(o) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The unequal-value control for the same Object-argument spelling.
BEZEL_TEST(decimal_equals_4562, decimal_equals_object_arg_false_unequal_spec_8_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 9.25m; "
        "  var d: decimal := 5.5m; "
        "  if d.Equals(o) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §8.3 "box carries its type" (the same rule fix #4511/#4559 established):
// an `Object` holding a DIFFERENT runtime type (an int, not a decimal) is
// never equal to a decimal, however the numeric values compare.
BEZEL_TEST(decimal_equals_4562, decimal_equals_object_arg_false_wrong_type_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  var d: decimal := 5.0m; "
        "  if d.Equals(o) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// A plain (non-`Object`-annotated) `int` argument: `IEquatable<decimal>`
// does not admit `int`, so this reaches the universal `Equals(Object?)`
// member with an implicit boxing conversion (§5.1 "any type to Object").
// Never equal (different runtime type), and must not crash.
BEZEL_TEST(decimal_equals_4562, decimal_equals_int_arg_false_spec_8_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 5.0m; "
        "  var i: int := 5; "
        "  if d.Equals(i) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ── the operator spelling (`=`), a sanity control ──────────────────────────

// §6.1/§8.3: `d = e` for the SAME issue-repro pair goes through
// `lowerBinary`'s decimal field-equality arm (LlvmBackend_Layout.cpp), a
// DIFFERENT function than the one this fix touches. Pinned to confirm the
// operator spelling already agreed with the correct answer even while
// `.Equals` was broken — i.e. the two spellings of §8.3's one rule now
// necessarily agree, rather than merely both happening to say `true`.
BEZEL_TEST(decimal_equals_4562, decimal_operator_eq_agrees_control_spec_6_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 5.5m; "
        "  var e: decimal := 5.50m; "
        "  if d = e then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── GetHashCode: must agree with Equals (§8.3's stated invariant) ─────────

// §8.3/§9.1: "if a.Equals(b) then a.GetHashCode() = b.GetHashCode()".
// Two different-scale-but-equal decimals must hash identically — this is
// the row that would catch a fix that made `Equals` correct while leaving
// `GetHashCode`'s pre-fix `nullptr` (and the caller's likely-zero fallback)
// unfixed, which would silently violate the hashing-coherence invariant.
BEZEL_TEST(decimal_equals_4562, decimal_gethashcode_agrees_with_equals_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 5.5m; "
        "  var e: decimal := 5.50m; "
        "  if (d.Equals(e)) and (d.GetHashCode() = e.GetHashCode()) then "
        "    return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The complementary control: an unequal pair need not (and, per the runtime
// hash's design, does not for this pair) collide.
BEZEL_TEST(decimal_equals_4562, decimal_gethashcode_differs_for_unequal_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 5.5m; "
        "  var e: decimal := 9.25m; "
        "  if d.GetHashCode() <> e.GetHashCode() then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── a real consumer of both: Dictionary<decimal, V> / Array<decimal> ──────

// stdlib §4.8: `Dictionary<K, V>` requires `K: hashable` and dispatches key
// membership through `Equals`/`GetHashCode` on a bare generic type parameter
// `K` bound to `decimal` — the SAME `lowerValueSemMethodOn` arm this fix
// touches, reached through the #1091 bare-type-parameter substitution path
// rather than the concrete-receiver path the rows above exercise. A
// dictionary keyed by decimal was silently wrong before this fix: every
// lookup's internal `Equals` answered `false`, so `ContainsKey` on a key that
// was plainly just inserted would itself have answered `false`.
BEZEL_TEST(decimal_equals_4562, dictionary_decimal_key_containskey_true_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var m: Dictionary<decimal, int> := [5.5m: 1, 9.25m: 2]; "
        "  if m.ContainsKey(5.50m) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

BEZEL_TEST(decimal_equals_4562, dictionary_decimal_key_containskey_false_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var m: Dictionary<decimal, int> := [5.5m: 1]; "
        "  if m.ContainsKey(9.25m) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// NOTE — `Array<decimal>.Contains(...)` was ALSO measured during this issue's
// grid sweep and found broken, but through a COMPLETELY DIFFERENT function
// (`emitRemoveElementEquality`, LlvmBackend_Calls.cpp) than the one this fix
// touches (`lowerValueSemMethodOn`, LlvmBackend_Calls2.cpp) — its scalar
// fallback arm never gained a decimal-struct case the way this fix's arm
// just did, and unlike THIS issue's silent-false defect it is a COMPILE-TIME
// CRASH ("Invalid operand types for ICmp instruction … icmp eq %bz.decimal"
// — IR verification failure), confirmed via a standalone probe compile.
// Filed separately (issue #4571) rather than widened into this lane, per
// CLAUDE.md's "no while-we're-at-it expansions" — it is a different
// receiver-comparison dispatch, not a mismatched-shape argument to
// `Equals`/`GetHashCode` on a decimal receiver.

// ── leak safety of the NEW box+dispatch call site (loop-amplified; a
// one-shot LSan row passes vacuously — CLAUDE.md "Oracles that pass
// vacuously") ────────────────────────────────────────────────────────────

// §8.6/§28.9: `lowerValueSemUniversalEquals` mints and releases the
// RECEIVER's box itself (pre-existing, unchanged by this fix); this row is
// the new CALL SITE reaching it — a decimal receiver's `.Equals(Object)`
// mismatched-shape spelling — looped so a leak at this new site is visible
// under ASan rather than passing on a live stack slot.
const char* kLoopedDecimalEqualsObject =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 300 do "
    "      var o: Object := 5.5m; "
    "      var d: decimal := 5.5m; "
    "      if d.Equals(o) then t := t + 1; end if; "
    "      k := k + 1; "
    "    end while; "
    "    if t = 300 then return 7; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

BEZEL_TEST(decimal_equals_4562, looped_decimal_equals_object_box_released_spec_8_6) {
    JitResult r = runAotChecked(kLoopedDecimalEqualsObject);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(decimal_equals_4562, looped_decimal_equals_object_plain_invariant_spec_8_1) {
    JitResult r = runAot(kLoopedDecimalEqualsObject);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
