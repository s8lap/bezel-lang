// Codegen — issue #4580: a NON-nullable `in`/`Contains`/`IndexOf` query
// operand against a NULLABLE-VALUE element type aborts the compiler in IR
// verification instead of using §5.1's implicit `T` → `T?` conversion and
// §8.4's null-safe equality (§17.5/§8.4/§5.1, v0.306).
//
// AUTHORITY. §5.1 v0.306, "Nullable conversions": "For any type `T`, `T`
// implicitly converts to `T?` … for a value type, the present-value wrapping
// ([§7.2])." §17.5 v0.306: "the left operand is admitted exactly when it is
// admissible as [the `Contains`] call's argument under the ordinary
// conversion rules ([§5.1])… Where the selected `Contains` parameter is
// itself nullable — `Array<int?>`'s `Contains(item: int?)` … — a nullable
// left operand is admitted: the test performed is the null-safe equality of
// [§8.4], total over an absent operand." A bare non-nullable operand (`3`)
// converts to `int?` by §5.1 before this rule even engages, so `3 in xs` over
// `Array<int?>` must be admitted and answered by §8.4's present-vs-present
// case (the inner type's equality).
//
// THE BUG. `Emitter::emitRemoveElementEquality`'s nullable-value arm (narrow
// #4553, LlvmBackend_Calls.cpp) fired only when BOTH operands already arrived
// as the tagged `{i8 hasValue, T value}` struct:
//
//   if (isNullableValueTy(elemTy) && el->getType()->isStructTy() &&
//       target->getType()->isStructTy())
//       return emitValueEquality(el, target, elemTy);
//
// The stored element `el` always does (loaded at the element's own stride),
// but a bare non-nullable query — `3 in xs`, or `.Contains(3)` — lowers
// straight from its `int` type (`lowerExpr`'s IntLiteralExpr arm reads
// `rt->primitive` and ignores `rt->nullable`), arriving as plain `i32`. The
// arm above declined, the decimal arm declined, the string arm needs two
// pointers, so it fell to the scalar fallback, which built
// `icmp eq { i8, i32 } %el, i32 %tgt` — an operand shape `ICmpInst` rejects
// outright, so the whole compile died in IR verification ("Both operands to
// ICmp instruction are not of the same type!"). Measured on this worktree's
// pre-fix HEAD (6f252b1d) with both repros from issue #4580.
//
// THE FIX coerces EACH operand up to the tagged struct with
// `ensureNullableElementValue` (the helper fix #4579 extracted for the
// sibling insert-side defect) before dispatching to `emitValueEquality` —
// the SAME per-value helper the #4553 arm already routed through, so this is
// not a fourth shape-specific comparison but the existing arm widened to
// admit an uncoerced operand on either side. `ensureNullableElementValue` is
// a no-op when its input is already the tagged struct, so the pre-existing
// both-tagged case (covered by in_nullable_value_element_4553_test.cpp) is
// unaffected — confirmed below as a control (present_query_against_present_
// element_already_wrapped_control).
//
// FAIL-WITHOUT proved mechanically: with LlvmBackend_Calls.cpp restored to
// this worktree's pre-fix HEAD (6f252b1d) and the test binary rebuilt, every
// `array_*`/`linkedlist_*` row below (excluding the *_control row, which is
// unaffected by the bug) reports `ran == false` (a codegen IR-verification
// crash), so `run()` yields the -999 sentinel against an expected 0 or 1.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── Array<int?>: bare non-nullable query, `x in coll` spelling ──────────────

// §5.1/§17.5: a bare present query converts to `int?` and matches a present
// slot of equal payload.
BEZEL_TEST(in_bare_operand_nullable_element_4580, array_bare_present_in_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs: Array<int?> := [1, null, 3]; "
        "  if 3 in xs then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.4: a bare present query absent from the array is NOT a member — the row
// that a fix returning a constant `true` (or `did not crash`) would fail.
BEZEL_TEST(in_bare_operand_nullable_element_4580, array_bare_absent_in_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs: Array<int?> := [1, null, 3]; "
        "  if 7 in xs then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §8.4: a bare present query is NOT equal to the array's own null slot —
// pins that the fix compares tags, not just payload bytes (a null slot's
// payload is zero-filled, so an implementation that ignored the tag would
// wrongly match `0`... but also must not match ANY present query against the
// null slot).
BEZEL_TEST(in_bare_operand_nullable_element_4580, array_bare_query_not_equal_to_null_slot_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs: Array<int?> := [null, 1, 2]; "
        "  if 0 in xs then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ── Array<int?>: the `.Contains`/`.IndexOf` named-member spellings ──────────

// §17.5: "`x in collection` evaluates to `true` if `collection.Contains(x)`
// is true" — the explicit call with a bare operand must agree.
BEZEL_TEST(in_bare_operand_nullable_element_4580, array_bare_contains_spec_17_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs: Array<int?> := [1, null, 3]; "
        "  if xs.Contains(3) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.4: `IndexOf` with a bare operand finds the present slot at its own
// position (2), not 0 and not missed (-1).
BEZEL_TEST(in_bare_operand_nullable_element_4580, array_bare_indexof_position_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs: Array<int?> := [1, null, 3]; "
        "  return xs.IndexOf(3); "
        "end Main; end C;"), 2);
}

// ── LinkedList<int?>: bare non-nullable query, both spellings ───────────────

// §5.1/§17.5: the LinkedList backing walks its nodes through the same
// `emitRemoveElementEquality` helper, so it carried the identical defect.
BEZEL_TEST(in_bare_operand_nullable_element_4580, linkedlist_bare_present_in_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var ll := new LinkedList<int?>(); "
        "  ll.AddLast(1); "
        "  ll.AddLast(null); "
        "  ll.AddLast(3); "
        "  if 3 in ll then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.4: the inverse control — a value the list does NOT hold is not a
// member, so a "fix" that made the coerced comparison constant-true fails
// here.
BEZEL_TEST(in_bare_operand_nullable_element_4580, linkedlist_bare_absent_in_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var ll := new LinkedList<int?>(); "
        "  ll.AddLast(1); "
        "  ll.AddLast(null); "
        "  ll.AddLast(3); "
        "  if 7 in ll then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §17.5: the `.Contains` named-member spelling on LinkedList with a bare
// operand.
BEZEL_TEST(in_bare_operand_nullable_element_4580, linkedlist_bare_contains_spec_17_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var ll := new LinkedList<int?>(); "
        "  ll.AddLast(1); "
        "  ll.AddLast(null); "
        "  ll.AddLast(3); "
        "  if ll.Contains(3) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── §8.4 null-safe rule via a null-holding array/list, bare-side control ────
//
// A literal `null` query is typed contextually to `int?` by sema BEFORE
// codegen sees it (CG23, LlvmBackend_Exprs.cpp: the NullLiteralExpr arm reads
// `ctx_.exprType(e)` and already produces the tagged null struct when that
// type is a nullable value type) — so it was NEVER the bare-operand shape
// #4580 is about, and this row is unaffected by the bug. Included as a
// control: the fix's `ensureNullableElementValue` call must be a no-op here,
// not change this pre-existing-correct answer.
BEZEL_TEST(in_bare_operand_nullable_element_4580, array_null_literal_query_control_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs: Array<int?> := [1, null, 3]; "
        "  if null in xs then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── control: both operands already tagged (pre-existing #4553 case) ─────────
//
// Confirms the widened arm does not disturb the case narrow #4553 already
// handled correctly — `ensureNullableElementValue` must be a no-op on a value
// that already has the tagged-struct shape.
BEZEL_TEST(in_bare_operand_nullable_element_4580, present_query_against_present_element_already_wrapped_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs: Array<int?> := [1, null, 3]; "
        "  var m: int? := 3; "
        "  if m in xs then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}
