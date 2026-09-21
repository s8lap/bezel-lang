// Codegen — issue #4553: `x in coll` and `coll.Contains(x)` over a NULLABLE
// VALUE element type perform §8.4's null-safe equality (§17.5 / §8.4, v0.306).
//
// AUTHORITY. §17.5 v0.306, verbatim: "Where the selected `Contains` parameter
// is **itself** nullable — `Array<int?>`'s `Contains(item: int?)`, or
// `Span<T>`/`ReadOnlySpan<T>` at class-typed `T` … — a nullable left operand is
// **admitted**: the test performed is the null-safe equality of §8.4, total
// over an absent operand and unaffected by §7.11." §8.4's rule is: both-null →
// equal, exactly-one-null → not-equal, both-present → the inner type's
// equality.
//
// THE BUG. `Emitter::emitRemoveElementEquality` — the one per-element
// comparison every array-, linked-list- and array-backed-set membership
// lowering funnels through — had no arm for a nullable value element. That
// element lowers to the tagged `{i8 hasValue, T value}` struct, which the
// record arm declines (not a Named type), the class arm declines (needs two
// pointers) and the string arm declines (needs two pointers), so it fell
// through to the scalar comparison and emitted `icmp eq { i8, i32 } %el, %tgt`.
// LLVM's verifier rejects that operand shape outright — "Invalid operand types
// for ICmp instruction" — so the whole compile died with `codegen error: IR
// verification failed`. Measured on b02083f3 with the hand-authored rule-corpus
// case `tests/conformance/rules/cases/s17_5_0004.bzl`, which this file's first
// row reproduces.
//
// FAIL-WITHOUT. Every row is a compile REJECT on the pre-fix compiler, so
// `runAot` reports `ran == false` and `run()` yields the -999 sentinel against
// the expected value. Verified mechanically by restoring
// src/codegen/LlvmBackend_Calls.cpp from origin/main and re-running this filter.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── `in` over Array<int?> (the rule corpus's s17_5_0004 repro) ──────────────

// §17.5/§8.4: an ABSENT left operand matches the array's own absent slot —
// "both-null → equal". This is the corpus case verbatim.
BEZEL_TEST(in_nullable_value_element_4553, null_in_array_with_null_spec_17_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs: Array<int?> := [1, null, 3]; "
        "  var n: int? := null; "
        "  if n in xs then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5/§8.4: a PRESENT left operand matches a present slot of equal payload.
BEZEL_TEST(in_nullable_value_element_4553, present_in_array_spec_17_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs: Array<int?> := [1, null, 3]; "
        "  var m: int? := 3; "
        "  if m in xs then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5/§8.4: a present operand absent from the array is NOT a member — the row
// that a fix returning a constant `true` would fail.
BEZEL_TEST(in_nullable_value_element_4553, present_absent_from_array_spec_17_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs: Array<int?> := [1, null, 3]; "
        "  var k: int? := 7; "
        "  if k in xs then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §8.4: "`null = nonNull` … `false`" — a null operand over an array whose slots
// are all present is NOT a member. Pins the tag half of the comparison: an
// implementation that compared only payloads would match the zero-filled slot
// of a present `0`, and an implementation that ignored tags entirely would say
// true here.
BEZEL_TEST(in_nullable_value_element_4553, null_not_in_all_present_array_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var ys: Array<int?> := [1, 2]; "
        "  var n: int? := null; "
        "  if n in ys then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ── the same helper reached through the named members (§17.5's own desugar) ──

// §17.5: "`x in collection` evaluates to `true` if `collection.Contains(x)` is
// true" — the explicit call must agree with the operator.
BEZEL_TEST(in_nullable_value_element_4553, contains_null_spec_17_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs: Array<int?> := [1, null, 3]; "
        "  var n: int? := null; "
        "  if xs.Contains(n) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.4: `IndexOf` compares elements through the same helper, so the absent slot
// is found at its own position (1), not at 0 and not missed (-1).
BEZEL_TEST(in_nullable_value_element_4553, indexof_null_position_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs: Array<int?> := [1, null, 3]; "
        "  var n: int? := null; "
        "  return xs.IndexOf(n); "
        "end Main; end C;"), 1);
}

// ── other element kinds and backings reached by the same helper ─────────────

// §10.3/§8.4: a nullable ENUM element is a nullable VALUE element — the same
// `{i8, <underlying>}` shape, so it took the same malformed `icmp`.
BEZEL_TEST(in_nullable_value_element_4553, nullable_enum_element_spec_17_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var cs: Array<Color?> := [Color.Red, null]; "
        "  var c: Color? := null; "
        "  var d: Color? := Color.Green; "
        "  if (c in cs) and not (d in cs) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5/§8.4: the LinkedList backing walks its nodes through the same
// comparison helper, so it carried the identical defect.
BEZEL_TEST(in_nullable_value_element_4553, linkedlist_nullable_element_spec_17_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var ll := new LinkedList<int?>(); "
        "  ll.AddLast(1); "
        "  ll.AddLast(null); "
        "  var n: int? := null; "
        "  if n in ll then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── #4579: the node-backed insert arms store the element at the TAGGED stride ──
//
// THE BUG (§4.6 / §7.2 / §17.5, v0.306). The row above found it under ASan:
// `LinkedList<int?>.AddLast(1)` spilled the bare inner `i32` into a 4-byte
// alloca and handed `bz_llist_add_last` the 8-byte stride `collectionElemSize`
// computes for `{i8 hasValue, i32 value}`, so `llist_node_alloc`'s memcpy read
// 4 bytes PAST the slot ("stack-buffer-overflow … READ of size 8 … in memcpy /
// llist_node_alloc / bz_llist_add_last", gate 75ae6094 shard codegen/5).
//
// But the over-read is the SECOND symptom. §7.2's tagged layout puts hasValue at
// offset 0, so the node's tag became the payload's LOW BYTE and the payload came
// from uninitialized stack. The rows below use 256 (low byte 0x00) so the stored
// element reads back as `null` — a WRONG ANSWER the plain oracle sees, which is
// why the row above passed plain and failed only under ASan (the #2633
// asymmetry). §17.5: "`x in collection` evaluates to `true` if
// `collection.Contains(x)` is true"; §8.4's present-vs-present case reduces to
// the inner type's equality, so each of these is `true`.
//
// FAIL-WITHOUT: all three return 0 on the pre-fix compiler (measured by probe on
// 75ae6094 and by restoring the two src files from HEAD); 1 with the fix.

// §4.6/§7.2/§17.5: AddLast stores a tagged element, not the bare inner bytes.
BEZEL_TEST(in_nullable_value_element_4553, linkedlist_addlast_tagged_stride_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var ll := new LinkedList<int?>(); "
        "  ll.AddLast(256); "
        "  var k: int? := 256; "
        "  if k in ll then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §4.6/§7.2/§17.5: the AddFirst arm carried the identical defect.
BEZEL_TEST(in_nullable_value_element_4553, linkedlist_addfirst_tagged_stride_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var ll := new LinkedList<int?>(); "
        "  ll.AddFirst(256); "
        "  var k: int? := 256; "
        "  if k in ll then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §4.6/§7.2/§17.5: and the two anchored arms — all four insert paths, so a fix
// that armed only the one the gate caught would fail here.
BEZEL_TEST(in_nullable_value_element_4553, linkedlist_addafter_addbefore_tagged_stride_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var ll := new LinkedList<int?>(); "
        "  var n1 := ll.AddLast(1); "
        "  ll.AddAfter(n1, 512); "
        "  ll.AddBefore(n1, 1024); "
        "  var a: int? := 512; "
        "  var b: int? := 1024; "
        "  if (a in ll) and (b in ll) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.4: the inverse control — a value the list does NOT hold is not a member, so
// a "fix" that made the tagged comparison constant-true fails here. `2 in ll`
// after storing 256 must be false.
BEZEL_TEST(in_nullable_value_element_4553, linkedlist_absent_value_is_not_member_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var ll := new LinkedList<int?>(); "
        "  ll.AddLast(256); "
        "  var k: int? := 2; "
        "  if k in ll then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}
