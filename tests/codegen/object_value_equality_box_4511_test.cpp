// Codegen — issue #4511: `Object = value` / `value = Object` boxes the value
// operand and dispatches `Object.Equals(Object?)` (§8.4 / §8.6 / §5.1, v0.306).
//
// AUTHORITY. §5.1's reference conversions include "any type to `Object`", a
// VALUE type reaching it by boxing (§8.6). §8.4 then fixes the meaning of the
// operator verbatim: "`a = b` invokes `a.Equals(b)`, with a null-safe wrapper:
// `null = null` is `true`; `null = nonNull` and `nonNull = null` are `false`.
// `<>` is the negation." §8.1 fixes `Object`'s member surface to include
// `Equals(Object?)`. The pair is therefore well-formed and sema admits it on
// exactly that authority — #4501's §5.1 operand-pair gate deliberately exempts
// `Object` on either side, pinned as a SEMA-only accept-control by
// `sema_binary_operand_pair_4501.t5_object_operand_not_rejected_by_this_rule_spec_5_1`
// (the sema verdict was right; the codegen was not).
//
// THE BUG. `lowerBinary` had no arm for the pair: the value operand was never
// boxed, so it reached the scalar fall-through and emitted
// `icmp eq ptr %o, i32 %i`, which LLVM's verifier rejects — "Both operands to
// ICmp instruction are not of the same type!" — so the whole compile died with
// `codegen error: IR verification failed`. Measured on b02083f3 for `Object =
// int`, `int = Object`, `Object <> int`, `Object = bool`, `Object = int?` and
// `Object = record`. A RECORD operand on the LEFT (`p = (Object)o`) was worse
// than malformed: it reached the §8.3 struct-equality dispatch with a POINTER
// on the other side and SEGFAULTED the compiler (exit 139).
//
// FAIL-WITHOUT. Every row below is a compile REJECT (or a compiler crash) on
// the pre-fix compiler, so `runAot` reports `ran == false` and `run()` yields
// the -999 sentinel against an expected 1. Verified mechanically by restoring
// src/codegen/LlvmBackend_Exprs.cpp from origin/main and re-running this filter.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── the issue's own repro, both orders ──────────────────────────────────────

// §8.4/§8.6 (#4511): `Object = int` — the boxed `5` and the boxed argument
// carry the same box vtable and the same payload, so `Equals` answers true.
BEZEL_TEST(object_value_equality_box_4511, object_eq_int_is_true_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  var i: int := 5; "
        "  if o = i then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.4/§8.6 (#4511): the other order. §8.4 makes the LEFT operand the receiver
// (`a.Equals(b)`), so here the `int` is boxed into the receiver position.
BEZEL_TEST(object_value_equality_box_4511, int_eq_object_is_true_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  var i: int := 5; "
        "  if i = o then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.4: `<>` is the negation — unequal payloads, same type.
BEZEL_TEST(object_value_equality_box_4511, object_ne_int_unequal_is_true_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  var j: int := 6; "
        "  if o <> j then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.4: and the positive form of the same pair answers false. This row is the
// one that would survive a fix that simply returned a constant `true`.
BEZEL_TEST(object_value_equality_box_4511, object_eq_int_unequal_is_false_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  var j: int := 6; "
        "  if o = j then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §8.6: the box carries its type, so a boxed *string* `\"5\"` is not equal to
// the int `5` — the comparison discriminates on the box vtable, not on any
// numeric or textual coincidence.
BEZEL_TEST(object_value_equality_box_4511, object_string_ne_int_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := \"5\"; "
        "  var i: int := 5; "
        "  if o = i then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §8.4/§8.6: a `bool` operand — a second primitive payload kind, so the row is
// not specific to `i32`.
BEZEL_TEST(object_value_equality_box_4511, object_eq_bool_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := true; "
        "  var t: bool := true; "
        "  var f: bool := false; "
        "  if (o = t) and not (o = f) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── nullable value operands (§8.6 Nullable value-type boxing + §8.4) ────────

// §8.6 "Nullable value-type boxing": a PRESENT `int?` boxes to a box of its
// payload and compares equal to the same boxed payload; an ABSENT one boxes to
// the null reference, which §8.4's null-safe wrapper makes unequal to a
// non-null `Object`.
BEZEL_TEST(object_value_equality_box_4511, object_eq_nullable_int_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  var n: int? := 5; "
        "  var z: int? := null; "
        "  if (o = n) and not (o = z) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.4: "`null = null` is `true`" — a null `Object?` against a null `int?`,
// whose §8.6 boxing is likewise the null reference.
BEZEL_TEST(object_value_equality_box_4511, null_object_eq_null_nullable_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object? := null; "
        "  var z: int? := null; "
        "  if o = z then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── record operand: the row that SEGFAULTED the compiler pre-fix ────────────

// §8.4/§8.6/§8.3 (#4511): a boxed positional record compares by the record's
// structural equality through the box's `Equals` thunk, in BOTH operand orders.
// `p = o` (record on the left) crashed the compiler outright before the fix.
BEZEL_TEST(object_value_equality_box_4511, object_record_both_orders_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var o: Object := new P(1, 2); "
        "  var p: P := new P(1, 2); "
        "  var q: P := new P(9, 9); "
        "  if (o = p) and (p = o) and not (o = q) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── control: the all-reference pair is untouched ────────────────────────────

// §4.6: `string` is a REFERENCE type that keeps its bare `bz_string*` in an
// `Object` slot, so `Object = string` never reaches the boxing arm. Pinned so a
// future widening of that arm cannot silently claim this pair.
BEZEL_TEST(object_value_equality_box_4511, object_eq_string_unchanged_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := \"hi\"; "
        "  var s: string := \"hi\"; "
        "  if o = s then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}
