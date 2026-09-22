// Codegen — issue #4573: `Emitter::emitRemoveElementEquality`'s arm list was an
// OPEN set, so a collection element whose LLVM shape matched no arm fell into a
// scalar `ICmpEQ`/`FCmpOEQ` tail and either crashed the compiler in IR
// verification or answered a silently wrong value (§8.3/§8.4/§4.3.1/§25.2,
// v0.306).
//
// MECHANISM. Every membership lowering shares one funnel —
// `Array<T>`/`OwningArray<T>`'s `.Contains`/`.IndexOf`/`.Remove`, `x in coll`
// (§17.5), the span scans, the `LinkedList<T>` node walk, and the array-backed
// `Set<T>`/`OwningSet<T>` dedup insert all call
// `Emitter::emitRemoveElementEquality` (src/codegen/LlvmBackend_Calls.cpp).
// That function used to dispatch on the OPERANDS' CURRENT LLVM SHAPE:
//
//   if (recordTypeOfRef(elemTy))                      -> emitStructEquality
//   if (elemIsClass && both pointers)                 -> Equals / identity
//   if (isNullableValueTy && both tagged structs)     -> emitValueEquality
//   if (decimal && both %bz.decimal)                  -> emitValueEquality
//   if (both pointers)                                -> emitStringEq   (!)
//   ... scalar tail: SExt-widen, FCmpOEQ, ICmpEQ
//
// Two failure modes follow structurally, and the grid below measures both on
// this worktree's pre-fix HEAD (cafdfc52):
//
//   (1) A struct-shaped element that matched no arm reached `CreateICmpEQ`,
//       which `ICmpInst` rejects for a struct/array operand — so the whole
//       compile died at the verifier ("Invalid operand types for ICmp
//       instruction"). Four arms had already been hand-added on exactly this
//       discovery path (#4553 nullable-value, #4571 decimal, #4579 the
//       insert-side sibling, #4580 the bare query operand); MEASURED still
//       crashing pre-fix: `Guid` (`icmp eq %bz.guid`), a value-like `choice`
//       (`icmp eq %choice.Sh`), `Range<int>` (`icmp eq %bz.range`),
//       `ArraySpan<int>` (`icmp eq %bz.arrayspan`), and an inline-array
//       element `[2]int`.
//
//   (2) The `both pointers -> emitStringEq` arm claimed EVERY pointer-shaped
//       element, not only `string`. `emitStringEq` reads word 1 of each
//       operand as a `%bz.string` code-point count (`loadStringLen`) and
//       memcmp's on it, so a boxed `Object`, a function value and a nested
//       builtin collection were compared as if their unrelated heap words were
//       string headers. MEASURED pre-fix: `Array<Object>{1,2}.Contains(5)`
//       answered `true`, and `Array<(int) -> int>{f}.Contains(g)` answered
//       `true` for a DIFFERENT function. (Both are ALLOCATOR-DEPENDENT — the
//       same two programs answered `false` when compiled with
//       `--sanitize=address`, #2633's "a different oracle, not a stronger one"
//       in miniature — so those two rows are pinned as CORRECTNESS oracles and
//       their pre-fix failure is a default-build measurement.)
//
// Two more gaps the same shape-driven order produced, both measured pre-fix:
//
//   (3) `recordTypeOfRef` ignores `nullable`, and its arm ran FIRST, so a
//       NULLABLE RECORD element took the record arm rather than #4553's
//       nullable-value arm: `emitStructEquality` compared the tagged
//       `{i8 hasValue, <payload>}` struct's field 0 (the TAG) against the bare
//       record's field 0. `Array<P?>{P(1)}.Contains(P(99))` therefore answered
//       `true` (tags both present), and `.Contains(null)` built
//       `icmp eq i8 %tag, i32 %x` and died in IR verification.
//
//   (4) The scalar tail's float compare was a naive `FCmpOEQ`, so
//       `Array<double>`/`Array<float>` were the last spelling of primitive
//       value equality in the compiler still strict-IEEE rather than
//       §4.3.1-reflexive ("`NaN = NaN` is true" — a deliberate rule, motivated
//       in §4.3.1 by `Set<double>`/`Dictionary<double, V>` needing NaN to be
//       findable once inserted). `emitValueEquality` has carried the
//       NaN-reflexive form since #514/#1165; this funnel never picked it up.
//
// THE FIX dispatches on the element's SEMA type, resolved once through the
// active monomorphization (§14.1), and COERCES both operands up front instead
// of inspecting shapes and declining on mismatch; the whole value family is
// handed to `emitValueEquality` — the single canonical per-value equality
// helper that `=`, record-field equality, the dictionary/set key trampolines
// (#1464) and `lowerValueSemMethodOn` (#4562) already share, and a strict
// superset of the retired tail. `Object` routes to `lowerObjectEquals` (the
// §9.6 helper #946/#947 built for `o.Equals(x)` at an `Object` receiver), which
// is §8.4's `a.Equals(b)` through the box's `bz$boxeq$<T>` vtable slot; every
// other non-class reference-shaped element gets §8.3's reference-type default,
// identity. "A new element kind with no arm" is now a visible hole in one
// enumeration rather than malformed IR.
//
// ORACLES. Every row below asserts a VALUE — `Contains` true/false, `IndexOf`
// the index, `Remove`'s effect on `Count` and the surviving element — never
// "it compiles". Each positive row is paired with an absent-value control, so a
// fix that answered `true` unconditionally would fail half the grid.
//
// FAIL-WITHOUT proved mechanically: with src/codegen/LlvmBackend_Calls.cpp
// restored to cafdfc52 and the test binary rebuilt, the rows noted in the
// section headers below fail; with the fix, all rows pass. The control rows
// (int / string / string? / record / class / interface / enum / decimal / int?
// / DateTime / generic `T`) pass on BOTH sides — they are what proves this
// refactor did not move the shapes that already worked.
//
// OUT OF SCOPE, measured and filed separately (NOT regressions of this fix,
// and unchanged on both sides of it):
//   * `Array<Guid?>` / `Array<DateTime?>` / `Array<Range<int>?>` store garbage
//     in the element slot — `a[0]` reads back neither the stored value nor
//     `null`, while `Guid? = Guid?` on two locals is correct. A nullable
//     value-BUILTIN element-stride defect upstream of any equality funnel.
//   * `Set<double>` NaN dedup goes through the generic-dict hash trampolines,
//     not this funnel (issue #4586).
//
// §8.3 / §8.4 / §4.3.1 / §25.2 / §17.5 / §35.7 / §15.1.2, v0.306.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── (1) struct-shaped elements that used to CRASH in IR verification ────────
// All five rows report `ran == false` pre-fix (the -999 sentinel): the compile
// dies in `Driver::compileFile` before a program exists to run.

// stdlib §2.6: `Guid` is a 128-bit value type lowered to `{i64 lo, i64 hi}` —
// the issue's own repro.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_guid_contains_present_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var g1: Guid := Guid.NewGuid(); "
        "  var a: Array<Guid> := [g1]; "
        "  if a.Contains(g1) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The absent control: two freshly minted Guids are distinct (stdlib §2.6), so
// a structural compare must answer false. A fix that answered `true`
// unconditionally passes the row above and fails this one.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_guid_contains_absent_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var g1: Guid := Guid.NewGuid(); "
        "  var g2: Guid := Guid.NewGuid(); "
        "  var a: Array<Guid> := [g1]; "
        "  if a.Contains(g2) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// `.IndexOf` pins the POSITION, not merely membership.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_guid_indexof_position_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var g1: Guid := Guid.NewGuid(); "
        "  var g2: Guid := Guid.NewGuid(); "
        "  var a: Array<Guid> := [g1, g2]; "
        "  if (a.IndexOf(g2) = 1) and (a.IndexOf(g1) = 0) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// `.Remove` pins the effect: the matched element goes, the other stays.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_guid_remove_leaves_other_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var g1: Guid := Guid.NewGuid(); "
        "  var g2: Guid := Guid.NewGuid(); "
        "  var a: Array<Guid> := [g1, g2]; "
        "  if a.Remove(g1) and (a.Count = 1) and (a[0] = g2) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// `x in coll` (§17.5) reaches the same funnel from a different lowering.
BEZEL_TEST(element_equality_sema_dispatch_4573, guid_in_array_spec_17_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var g1: Guid := Guid.NewGuid(); "
        "  var g2: Guid := Guid.NewGuid(); "
        "  var a: Array<Guid> := [g1]; "
        "  if (g1 in a) and (not (g2 in a)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The node-backed `LinkedList<T>` walk (a different function in a different
// file, LlvmBackend_Calls3.cpp) shares the funnel.
BEZEL_TEST(element_equality_sema_dispatch_4573, linkedlist_guid_contains_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var g1: Guid := Guid.NewGuid(); "
        "  var g2: Guid := Guid.NewGuid(); "
        "  var l: LinkedList<Guid> := [g1]; "
        "  if l.Contains(g1) and (not l.Contains(g2)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §35.7: a value-like auto-hashable choice compares by active case, then
// payload. `emitValueEquality` routes it to `emitChoiceEquality`; the retired
// tail emitted `icmp eq %choice.Sh`.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_choice_contains_by_case_and_payload_spec_35_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; choice Sh A(n: int), B; end Sh; "
        "class C public static method Main(): int "
        "  var a: Array<Sh> := [Sh.A(1), Sh.B]; "
        "  if a.Contains(Sh.A(1)) and a.Contains(Sh.B) "
        "     and (not a.Contains(Sh.A(9))) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// stdlib §9.8: `Range<T>` is a compiler-intrinsic body record lowered to
// `{i64 start, i64 end, i64 step, i8 flags}`.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_range_contains_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r1: Range<int> := 1 .. 5; "
        "  var r2: Range<int> := 2 .. 7; "
        "  var a: Array<Range<int>> := [r1]; "
        "  if a.Contains(r1) and (not a.Contains(r2)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §4.15: `ArraySpan<T>` is the borrowing `{ptr source, i32 start, i32 length}`
// value record; §8.3's positional-record row gives it field-by-field equality.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_arrayspan_contains_spec_4_15) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var src: Array<int> := [1, 2, 3]; "
        "  var s1: ArraySpan<int> := src.AsSpan(0, 2); "
        "  var s2: ArraySpan<int> := src.AsSpan(1, 2); "
        "  var a: Array<ArraySpan<int>> := [s1]; "
        "  if a.Contains(s1) and (not a.Contains(s2)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §15.1.2: an inline-array element compares element-by-element.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_inline_array_element_contains_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: [2]int := [1, 2]; "
        "  var y: [2]int := [1, 9]; "
        "  var a: Array<[2]int> := [x]; "
        "  if a.Contains(x) and (not a.Contains(y)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── (3) a NULLABLE RECORD element: the record arm ran before the nullable arm
// and compared the tagged struct's hasValue TAG ─────────────────────────────

// Pre-fix this answered `true` (both tags present) — a SILENT wrong answer, the
// worst of the four modes because nothing reports it.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_nullable_record_absent_payload_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(X: int); "
        "class C public static method Main(): int "
        "  var a: Array<P?> := [new P(1)]; "
        "  var q: P? := new P(99); "
        "  if a.Contains(q) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// The present-payload half of the same rule: equal payloads must match.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_nullable_record_present_payload_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(X: int); "
        "class C public static method Main(): int "
        "  var a: Array<P?> := [new P(1), null]; "
        "  var q: P? := new P(1); "
        "  if a.Contains(q) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The same element with a STRING field — pre-fix the tag compare hid the
// payload entirely, so unequal strings matched.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_nullable_record_string_field_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; record S(N: string); "
        "class C public static method Main(): int "
        "  var a: Array<S?> := [new S(\"ab\")]; "
        "  var same: S? := new S(\"a\" + \"b\"); "
        "  var diff: S? := new S(\"zz\"); "
        "  if a.Contains(same) and (not a.Contains(diff)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.4's null-safe rule over the same element: a bare `null` LITERAL query.
// Pre-fix this built `icmp eq i8 %tag, i32 %x` and died in IR verification.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_nullable_record_null_literal_query_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(X: int); "
        "class C public static method Main(): int "
        "  var withNull: Array<P?> := [new P(1), null]; "
        "  var noNull: Array<P?> := [new P(1)]; "
        "  if withNull.Contains(null) and (not noNull.Contains(null)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── (4) §4.3.1's reflexive NaN equality ─────────────────────────────────────

// §4.3.1: "`NaN = NaN` is true". Pre-fix the funnel's `FCmpOEQ` answered false
// while `emitValueEquality` (and therefore `=`) answered true — one language,
// two answers for the same comparison.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_double_contains_nan_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var z: double := 0.0; "
        "  var a: Array<double> := [z / z, 1.0]; "
        "  var n: double := z / z; "
        "  if a.Contains(n) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The ordinary-value control: reflexivity must not make everything equal.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_double_contains_absent_control_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<double> := [1.5, 2.5]; "
        "  if a.Contains(1.5) and (not a.Contains(9.5)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// `.IndexOf` over the same rule: position 1, not -1.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_double_indexof_nan_position_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var z: double := 0.0; "
        "  var a: Array<double> := [1.0, z / z]; "
        "  var n: double := z / z; "
        "  if a.IndexOf(n) = 1 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// `in` over the same rule (§17.5's lowering).
BEZEL_TEST(element_equality_sema_dispatch_4573, nan_in_array_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var z: double := 0.0; "
        "  var a: Array<double> := [z / z]; "
        "  var n: double := z / z; "
        "  if n in a then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// `float` is the same rule at 32 bits.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_float_contains_nan_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var z: float := 0.0f; "
        "  var a: Array<float> := [z / z, 1.0f]; "
        "  var n: float := z / z; "
        "  if a.Contains(n) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// `Array<double?>` already routed through `emitValueEquality` (#4553's arm) and
// was therefore ALREADY NaN-reflexive pre-fix — the internal inconsistency this
// fix removes. A control on both sides, and the evidence that the funnel, not
// the rule, was the divergence.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_nullable_double_nan_control_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var z: double := 0.0; "
        "  var a: Array<double?> := [z / z, null]; "
        "  var n: double? := z / z; "
        "  if a.Contains(n) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── (2) pointer-shaped elements the `emitStringEq` catch-all claimed ────────

// §8.4 / §8.3's Primitive row: a boxed value's `Equals` is value comparison, so
// `Contains` over boxed ints answers by VALUE. Pre-fix this read the boxes'
// second word as a string length; the absent row answered `true` on the default
// build and `false` under ASan (allocator-dependent garbage).
BEZEL_TEST(element_equality_sema_dispatch_4573, array_object_boxed_value_equality_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Object> := [1, 2]; "
        "  var present: Object := 2; "
        "  var absent: Object := 5; "
        "  if a.Contains(present) and (not a.Contains(absent)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// A string stored at an `Object` element still content-compares (§4.6) — the
// `lowerObjectEquals` string leg, so routing `Object` away from the raw
// `emitStringEq` did not cost the case that arm got right.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_object_string_content_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Object> := [\"ab\", \"cd\"]; "
        "  var q: Object := \"a\" + \"b\"; "
        "  if a.Contains(q) and (not a.Contains(\"zz\")) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.3's reference default for a FUNCTION VALUE: identity. Pre-fix
// `Contains(g)` answered `true` for a different function on the default build.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_function_value_identity_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Add1(x: int): int return x + 1; end Add1; "
        "  public static method Add2(x: int): int return x + 2; end Add2; "
        "  public static method Main(): int "
        "    var f: (int) -> int := Add1; "
        "    var g: (int) -> int := Add2; "
        "    var a: Array<(int) -> int> := [f]; "
        "    if a.Contains(f) and (not a.Contains(g)) then return 1; end; "
        "    return 0; "
        "  end Main; end C;"), 1);
}

// A nested builtin collection element: `Array<Array<int>>`. §8.3's reference
// default again — the inner arrays have no `%bz.string` header for the retired
// arm's `loadStringLen` to read.
BEZEL_TEST(element_equality_sema_dispatch_4573, array_nested_collection_identity_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var i1: Array<int> := [1, 2]; "
        "  var i2: Array<int> := [3]; "
        "  var i3: Array<int> := [7, 8, 9]; "
        "  var a: Array<Array<int>> := [i1, i2]; "
        "  if a.Contains(i2) and (not a.Contains(i3)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── CONTROLS: shapes that already worked, and must still ────────────────────
// These pass on BOTH sides of the fix. They are what proves the refactor moved
// only the broken shapes: this funnel is reached by `in`, `.Contains`,
// `.IndexOf`, `.Remove`, the span scans, the array-backed `Set<T>` dedup and
// the `LinkedList<T>` node walk, so a regression here would be broad and silent.

BEZEL_TEST(element_equality_sema_dispatch_4573, control_array_int_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3]; "
        "  if a.Contains(2) and (a.IndexOf(3) = 2) and (not a.Contains(9)) "
        "     then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §4.6: a `string` element content-compares two DISTINCT heap strings.
BEZEL_TEST(element_equality_sema_dispatch_4573, control_array_string_content_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<string> := [\"ab\", \"cd\"]; "
        "  var q: string := \"a\" + \"b\"; "
        "  if a.Contains(q) and (not a.Contains(\"zz\")) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// `string?` — content equality plus §8.4's null-safe rule.
BEZEL_TEST(element_equality_sema_dispatch_4573, control_array_nullable_string_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<string?> := [\"ab\", null]; "
        "  var q: string? := \"a\" + \"b\"; "
        "  if a.Contains(q) and a.Contains(null) and (not a.Contains(\"zz\")) "
        "     then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.3's positional-record row, including a nested record with a string field
// (§9.1 transitivity) — the arm that moved from an inlined `emitStructEquality`
// to `emitValueEquality`'s memoized per-instantiation helper (#4234).
BEZEL_TEST(element_equality_sema_dispatch_4573, control_array_record_structural_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Inner(S: string); record Outer(I: Inner); "
        "class C public static method Main(): int "
        "  var a: Array<Outer> := [new Outer(new Inner(\"ab\"))]; "
        "  var same: Outer := new Outer(new Inner(\"a\" + \"b\")); "
        "  var diff: Outer := new Outer(new Inner(\"zz\")); "
        "  if a.Contains(same) and (not a.Contains(diff)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.3's Class row: no `Equals` override → reference identity.
BEZEL_TEST(element_equality_sema_dispatch_4573, control_array_class_reference_identity_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class K public V: int; "
        "  public constructor(v: int) V := v; end constructor; end K; "
        "class C public static method Main(): int "
        "  var k1: K := new K(1); "
        "  var k2: K := new K(2); "
        "  var a: Array<K> := [k1, k2]; "
        "  if a.Contains(k2) and (not a.Contains(new K(1))) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §8.4: a class WITH an `Equals` override dispatches virtually.
BEZEL_TEST(element_equality_sema_dispatch_4573, control_array_class_equals_override_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class K public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public override method Equals(o: Object?): bool "
        "    var other: K? := o as K; "
        "    if other = null then return false; end; "
        "    return other.V = this.V; "
        "  end Equals; "
        "  public override method GetHashCode(): int return V; end GetHashCode; "
        "end K; "
        "class C public static method Main(): int "
        "  var a: Array<K> := [new K(1)]; "
        "  if a.Contains(new K(1)) and (not a.Contains(new K(7))) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// An INTERFACE element keeps §8.3 reference identity (the funnel's
// `elemIsClass` parameter covers Class|Interface since #619).
BEZEL_TEST(element_equality_sema_dispatch_4573, control_array_interface_identity_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; interface I method Get(): int; end I; "
        "class K: I public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public method Get(): int return V; end Get; end K; "
        "class C public static method Main(): int "
        "  var k1: I := new K(1); "
        "  var k2: I := new K(2); "
        "  var a: Array<I> := [k1, k2]; "
        "  if a.Contains(k2) and (not a.Contains(new K(1))) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §10: an enum element compares its underlying integer.
BEZEL_TEST(element_equality_sema_dispatch_4573, control_array_enum_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var a: Array<Color> := [Color.Red, Color.Blue]; "
        "  if a.Contains(Color.Blue) and (not a.Contains(Color.Green)) "
        "     then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §4.4 (#4571's arm, now reached through the value authority): scale-invariant.
BEZEL_TEST(element_equality_sema_dispatch_4573, control_array_decimal_scale_invariant_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<decimal> := [5.5m, 9.25m]; "
        "  if a.Contains(5.50m) and (not a.Contains(1.0m)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5 / §5.1 (#4553 + #4580): a nullable-value element, both a nullable and a
// BARE query operand, plus the `null` literal.
BEZEL_TEST(element_equality_sema_dispatch_4573, control_array_nullable_int_spec_17_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int?> := [1, null, 3]; "
        "  var q: int? := 3; "
        "  if a.Contains(q) and a.Contains(1) and a.Contains(null) "
        "     and (not a.Contains(7)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// stdlib §2.5: `DateTime` is a scalar i64 value builtin — already correct.
BEZEL_TEST(element_equality_sema_dispatch_4573, control_array_datetime_spec_2_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: DateTime := DateTime.UtcNow(); "
        "  var a: Array<DateTime> := [d]; "
        "  if a.Contains(d) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §14.1: the funnel inside a MONOMORPHIZED generic body. The element type is a
// bare `T` there, so the sema-type dispatch depends on resolving it through the
// active substitution — this row is what proves that resolution works for both
// a value and a reference instantiation.
BEZEL_TEST(element_equality_sema_dispatch_4573, control_generic_body_element_spec_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Has<T>(a: Array<T>, q: T): bool "
        "    return a.Contains(q); "
        "  end Has; "
        "  public static method Main(): int "
        "    var ints: Array<int> := [1, 2]; "
        "    var strs: Array<string> := [\"ab\"]; "
        "    if Has<int>(ints, 2) and (not Has<int>(ints, 9)) "
        "       and Has<string>(strs, \"a\" + \"b\") "
        "       and (not Has<string>(strs, \"zz\")) then return 1; end; "
        "    return 0; "
        "  end Main; end C;"), 1);
}

// The array-backed `Set<T>` dedup insert (stdlib §4.10) reaches the funnel from
// `emitArrayBackedSetInsert`, which passes `elemIsClass` unconditionally — a
// class element deduplicates on §8.3 reference identity.
BEZEL_TEST(element_equality_sema_dispatch_4573, control_set_class_element_dedup_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class K public V: int; "
        "  public constructor(v: int) V := v; end constructor; end K; "
        "class C public static method Main(): int "
        "  var k1: K := new K(1); "
        "  var s: Set<K> := [k1, k1, new K(1)]; "
        "  if s.Count = 2 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// `Set<Guid>` is GENERIC-DICT backed (`genericBackingType`, #1464's trampolines)
// and never reaches this funnel — a control that was already correct pre-fix
// and scopes the fix to the linear-scan family.
BEZEL_TEST(element_equality_sema_dispatch_4573, control_set_guid_generic_backed_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var g1: Guid := Guid.NewGuid(); "
        "  var s: Set<Guid> := [g1, g1]; "
        "  if s.Count = 1 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}
