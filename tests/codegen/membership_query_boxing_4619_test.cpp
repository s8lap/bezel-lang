// Codegen — issue #4619: a collection MEMBERSHIP QUERY whose static type is a
// bare value type was never §8.6-boxed at the call site, so `Array<Object>`'s
// `.Contains` / `.IndexOf` / `.Remove` / `x in coll` compared the query against
// the stored element's BOX HEADER instead of its payload (§8.6/§25.2/§8.4,
// v0.307).
//
// MECHANISM (re-derived on this lane's HEAD, bf7a7515, and confirmed from the
// emitted IR — not merely from the symptom). Every membership lowering handed
// `emitRemoveElementEquality` (src/codegen/LlvmBackend_Calls.cpp) the value
// `lowerExpr` produced for the query, at the QUERY's own type. #4573's Object
// arm (step 4) is gated on `bothPtr`, so a raw `i32` facing a boxed element
// pointer skipped it; so did the string arm and the reference-identity arm.
// Control reached the value-family tail, whose "one side is a spilled
// aggregate" coercion LOADS THROUGH THE POINTER OPERAND at the other side's
// type. For `var a: Array<Object> := [1, 2];` the emitted scan body was
// literally:
//
//     %el = load ptr, ptr %slot          ; the boxed element
//     %h  = load i32, ptr %el            ; ← the BOX HEADER, not the payload
//     %eq = icmp eq i32 %h, 1            ; low half of @"pvt$int"'s address
//
// The §8.6 box is `{ ptr vtable, i64 rc, payload }`, so the payload lives at
// offset 16 and word 0 is the box vtable's address — a value that essentially
// never equals a small integer. MEASURED pre-fix: `a.Contains(1)`,
// `a.Contains(2)` and `a.Contains(i)` all answered `False` for PRESENT
// elements, `a.IndexOf(2)` answered -1, and the same queries spelled through a
// pre-boxed `var o: Object := 1;` answered correctly (#4573's own regression
// test uses only that pre-boxed spelling, which is why this shape survived it).
//
// The defect is not "always false": with a `bool` query the coercion emits
// `load i1, ptr %el` — one BIT of the box vtable's address — and MEASURED
// pre-fix `Array<Object>{1, 2}.Contains(true)` answered **True** for a value
// that is not in the array. That row is pinned below as a correctness oracle;
// its pre-fix value is address-dependent, so the rows that carry this test's
// FAIL-WITHOUT weight are the deterministic present/absent/IndexOf ones.
//
// THE FIX. `Emitter::boxMembershipQuery` (src/codegen/LlvmBackend_Layout.cpp,
// beside `boxForTarget`) applies §8.6 — "when a value type is used where
// `Object` (or an interface implemented by the value type) is expected, the
// value is boxed" — to the query, ONCE per call and outside the per-element
// loop, at every lowering that feeds `emitRemoveElementEquality` a
// caller-supplied query: `Array<T>`/`OwningArray<T>`/`Stack<T>`/`Queue<T>`'s
// `.Contains`/`.IndexOf`/`.Remove`, `x in coll` (§17.5, both the array-backed
// and the `LinkedList<T>` arm), `LinkedList<T>.Find`/`.Remove`/`.Contains`,
// `ArraySpan<T>`/`ReadOnlyArraySpan<T>`'s `.IndexOf`/`.Contains`, and the
// inline-array (`[N]T`) `.Contains`/`in`. `boxForTarget` is a no-op for a
// reference source or a non-Object/interface element, so every already-correct
// shape is byte-identical — which the control rows below are here to prove.
// `emitRemoveElementEquality` additionally REFUSES the unboxed shape now (a
// reference element facing a non-pointer query answers "no match") rather than
// reading the header, so a lowering added later without the boxing step is
// deterministic instead of garbage-dependent.
//
// ORACLES. Every row asserts a VALUE — `Contains` true/false, `IndexOf`'s
// index, `Remove`'s effect on `Count` and on the surviving element — never "it
// compiles", and every present row is paired with an ABSENT control, so a fix
// that answered `true` unconditionally would fail half the grid.
//
// §8.6 / §8.4 / §8.3 / §25.2 / §17.5 / §15.1.2 / §4.6, v0.307.

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

// ── Array<Object>.Contains — the issue's own repro ──────────────────────────

// The issue's first two measured lines: `Array<Object>{1,2}.Contains(1)` and
// `.Contains(2)`, both PRESENT, both `False` pre-fix.
BEZEL_TEST(membership_query_boxing_4619, array_contains_bare_int_present_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Object> := [1, 2]; "
        "  if a.Contains(1) and a.Contains(2) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The absent control. Pre-fix this answered `False` too — correct, but by
// accident (the header word is not 5 either). It is here so that a fix which
// simply answered `true` cannot pass.
BEZEL_TEST(membership_query_boxing_4619, array_contains_bare_int_absent_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Object> := [1, 2]; "
        "  if a.Contains(5) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// The issue's fourth line: the same defect reached through a VARIABLE rather
// than a literal, which rules out a constant-folding explanation.
BEZEL_TEST(membership_query_boxing_4619, array_contains_bare_int_variable_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Object> := [1, 2]; "
        "  var i: int := 1; "
        "  var j: int := 5; "
        "  if a.Contains(i) and (not a.Contains(j)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The issue's fifth line, and this test's key CONTROL: a PRE-BOXED
// `Object`-typed query was already correct (it is the only spelling #4573's
// own test used) and must stay correct — a fix that double-boxed an
// already-`Object` source would break exactly this row.
BEZEL_TEST(membership_query_boxing_4619, array_contains_preboxed_object_control_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Object> := [1, 2]; "
        "  var o: Object := 1; "
        "  var p: Object := 5; "
        "  if a.Contains(o) and (not a.Contains(p)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The WRONG-ANSWER direction, and the row that proves the mechanism is a header
// read rather than "membership is always false": a `bool` query made the tail
// emit `load i1, ptr %el` — bit 0 of the box vtable's address — and
// `Array<Object>{1, 2}.Contains(true)` MEASURED **True** pre-fix. Paired with
// the genuinely-present case so both directions are pinned.
BEZEL_TEST(membership_query_boxing_4619, array_contains_bool_query_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Object> := [1, 2]; "
        "  var b: Array<Object> := [true]; "
        "  if (not a.Contains(true)) and (not a.Contains(false)) "
        "     and b.Contains(true) and (not b.Contains(false)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// Not an `int`-only defect: a `double` query against the same `Object` element
// slot takes the identical path (the tail would emit `load double, ptr %el`).
BEZEL_TEST(membership_query_boxing_4619, array_contains_double_query_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Object> := [1.5, 2.5]; "
        "  if a.Contains(2.5) and (not a.Contains(9.5)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── IndexOf — the same funnel, and a POSITION oracle rather than a boolean ──

BEZEL_TEST(membership_query_boxing_4619, array_indexof_bare_int_position_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Object> := [1, 2, 3]; "
        "  if (a.IndexOf(2) = 1) and (a.IndexOf(1) = 0) and (a.IndexOf(9) = -1) "
        "     then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── Remove — the same funnel; the oracle is the EFFECT on the collection ────

BEZEL_TEST(membership_query_boxing_4619, array_remove_bare_int_effect_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Object> := [1, 2]; "
        "  var gone: bool := a.Remove(1); "
        "  if gone and (a.Count = 1) and (not a.Contains(1)) and a.Contains(2) "
        "     then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── `x in coll` (§17.5) — a different lowering into the same funnel ─────────

BEZEL_TEST(membership_query_boxing_4619, in_operator_array_bare_int_spec_17_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Object> := [1, 2]; "
        "  if (2 in a) and (not (9 in a)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// `Stack<T>` is bz_array_t-backed and reaches the §17.5 array arm too.
BEZEL_TEST(membership_query_boxing_4619, in_operator_stack_bare_int_spec_17_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Stack<Object>(); "
        "  s.Push(42); "
        "  if (42 in s) and (not (7 in s)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── ArraySpan<T> (stdlib §4.14) — the span scan shares the funnel ───────────

BEZEL_TEST(membership_query_boxing_4619, span_contains_indexof_bare_int_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Object> := [7, 8]; "
        "  var sp := a.AsSpan(); "
        "  if sp.Contains(8) and (not sp.Contains(9)) and (sp.IndexOf(8) = 1) "
        "     then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── Inline arrays (§15.1.2) — `.Contains` and `in`, both arms ───────────────

BEZEL_TEST(membership_query_boxing_4619, inline_array_contains_bare_int_spec_15_1_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var ia: [3]Object := [4, 5, 6]; "
        "  if ia.Contains(5) and (not ia.Contains(9)) "
        "     and (5 in ia) and (not (9 in ia)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── Controls: shapes `boxForTarget` must leave byte-identical ───────────────

// A `string` query is already reference-shaped, so §8.6 does not box it and
// step 3's content equality (§4.6) answers. Unchanged by this fix.
BEZEL_TEST(membership_query_boxing_4619, array_object_string_query_control_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Object> := [\"ab\", \"cd\"]; "
        "  if a.Contains(\"ab\") and (not a.Contains(\"zz\")) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// A CLASS element slot is not a §8.6 boxing target, so `boxForTarget` declines
// and §8.3's reference identity still decides. Unchanged by this fix.
BEZEL_TEST(membership_query_boxing_4619, array_class_element_identity_control_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class K public V: int; end K; "
        "class C public static method Main(): int "
        "  using var k1 := new K(); using var k2 := new K(); "
        "  var a: Array<K> := [k1]; "
        "  if a.Contains(k1) and (not a.Contains(k2)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// A plain VALUE element slot is not a boxing target either — `Array<int>` must
// keep comparing payloads directly.
BEZEL_TEST(membership_query_boxing_4619, array_int_element_control_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [1, 2]; "
        "  if a.Contains(2) and (not a.Contains(9)) and (a.IndexOf(2) = 1) "
        "     then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}
