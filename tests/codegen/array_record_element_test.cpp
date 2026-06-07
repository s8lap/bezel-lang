// Codegen regression — Array<T> element-size for multi-field records (bug D).
//
// §9 / §4.4: a record is a value type stored by value; Array<T>.Get(i) returns
// T by value (a full copy of the element bytes). When T is a record with a
// reference field (e.g. string) followed by a value field (e.g. int), the
// record struct has two LLVM fields: a ptr (8 bytes) + i32 (4 bytes, padded to
// 8) = 16 bytes total.  The bug: `collectionElemSize` in LlvmBackend_Calls.cpp
// (and its duplicates in Stmts.cpp and Exprs.cpp) only recognised primitive
// scalar types and fell through to `return 8` for any StructType. For
// Array<Named> the stride used by bz_array_add/bz_array_get was therefore 8
// instead of 16, so Add stored only 8 of the 16 element bytes and Get loaded
// from a wrong offset — `it.Age` returned garbage (120) instead of 30.
//
// Spec coverage:
//   §9   — records are value types, copied by value.
//   §4.4 — Array<T>.Add(v) / .Get(i): T returns T by value.
//   §4.1 — for-in iteration is element-by-element (§9 copy semantics apply).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ---------------------------------------------------------------------------
// (a) Exact repro from the bug report: ref field (string) then value field
//     (int). §9 / §4.4.
// ---------------------------------------------------------------------------

// The primary repro: Named has string then int. Get(0).Age must be 30.
// Before the fix: returns 120 (garbage read from wrong offset).
BEZEL_TEST(array_record_element, ref_then_value_get_age_spec_9_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Named(Name: string, Age: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Named>(); "
        "  a.Add(new Named(\"Alice\", 30)); "
        "  var it := a.Get(0); "
        "  return it.Age; "
        "end Main; end C;"), 30);
}

// Get both fields of the first element. §9 copy must round-trip both fields.
BEZEL_TEST(array_record_element, ref_then_value_get_both_fields_spec_9_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Named(Name: string, Age: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Named>(); "
        "  a.Add(new Named(\"Alice\", 30)); "
        "  var it := a.Get(0); "
        "  return int(it.Name = \"Alice\" and it.Age = 30); "
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// (b) Multiple elements: Add 3, Get each, check both fields. §4.4.
// ---------------------------------------------------------------------------

// Three elements with distinct field values — each Get must return the
// correct element (not an overlapping/shifted read). §4.4.
BEZEL_TEST(array_record_element, ref_then_value_three_elements_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Named(Name: string, Age: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Named>(); "
        "  a.Add(new Named(\"Alice\", 10)); "
        "  a.Add(new Named(\"Bob\", 20)); "
        "  a.Add(new Named(\"Carol\", 30)); "
        "  var r0 := a.Get(0); var r1 := a.Get(1); var r2 := a.Get(2); "
        "  return int(r0.Name = \"Alice\" and r0.Age = 10 "
        "         and r1.Name = \"Bob\"   and r1.Age = 20 "
        "         and r2.Name = \"Carol\" and r2.Age = 30); "
        "end Main; end C;"), 1);
}

// Sum of Ages across three elements — exercises all three Get calls. §4.4.
BEZEL_TEST(array_record_element, ref_then_value_sum_ages_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Named(Name: string, Age: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Named>(); "
        "  a.Add(new Named(\"A\", 10)); "
        "  a.Add(new Named(\"B\", 20)); "
        "  a.Add(new Named(\"C\", 30)); "
        "  return a.Get(0).Age + a.Get(1).Age + a.Get(2).Age; "
        "end Main; end C;"), 60);
}

// ---------------------------------------------------------------------------
// (c) for-in over Array<Named> sums Ages. §4.1 / §9 copy semantics.
// ---------------------------------------------------------------------------

// for-in uses bz_array_ptr_at with the same esz; this exercises the Stmts.cpp
// path (the "rt.cond / rt.body" loop). §4.1 / §9.
BEZEL_TEST(array_record_element, ref_then_value_for_in_sum_ages_spec_4_1_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Named(Name: string, Age: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Named>(); "
        "  a.Add(new Named(\"Alice\", 10)); "
        "  a.Add(new Named(\"Bob\", 20)); "
        "  a.Add(new Named(\"Carol\", 30)); "
        "  var total := 0; "
        "  for var it in a do total := total + it.Age; end; "
        "  return total; "
        "end Main; end C;"), 60);
}

// for-in: check both fields of each element. §4.1 / §9.
BEZEL_TEST(array_record_element, ref_then_value_for_in_both_fields_spec_4_1_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Named(Name: string, Age: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Named>(); "
        "  a.Add(new Named(\"X\", 1)); "
        "  a.Add(new Named(\"Y\", 2)); "
        "  var ok := 1; "
        "  var i := 0; "
        "  for var it in a do "
        "    if i = 0 then "
        "      if not (it.Name = \"X\" and it.Age = 1) then ok := 0; end; "
        "    end; "
        "    if i = 1 then "
        "      if not (it.Name = \"Y\" and it.Age = 2) then ok := 0; end; "
        "    end; "
        "    i := i + 1; "
        "  end; "
        "  return ok; "
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// (d) Value-then-ref field order — must also work correctly. §9 / §4.4.
// ---------------------------------------------------------------------------

// Swapped order: int field first, string field second. §9 / §4.4.
BEZEL_TEST(array_record_element, value_then_ref_get_fields_spec_9_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Scored(Score: int, Label: string); "
        "class C public static method Main(): int "
        "  using var a := new Array<Scored>(); "
        "  a.Add(new Scored(42, \"first\")); "
        "  a.Add(new Scored(99, \"second\")); "
        "  var r0 := a.Get(0); var r1 := a.Get(1); "
        "  return int(r0.Score = 42 and r0.Label = \"first\" "
        "         and r1.Score = 99 and r1.Label = \"second\"); "
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// (e) Control cases: homogeneous-field records. §9 / §4.4.
// ---------------------------------------------------------------------------

// Two int fields — a record where the previous code happened to work
// (StructType but both fields i32 → stride still wrong at 8, but 8-byte reads
// happen to pick up the first field). After the fix, both fields must be
// correct. §9 / §4.4.
BEZEL_TEST(array_record_element, two_value_fields_get_both_spec_9_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair(X: int, Y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Pair>(); "
        "  a.Add(new Pair(3, 7)); "
        "  a.Add(new Pair(5, 11)); "
        "  var r0 := a.Get(0); var r1 := a.Get(1); "
        "  return int(r0.X = 3 and r0.Y = 7 and r1.X = 5 and r1.Y = 11); "
        "end Main; end C;"), 1);
}

// Two string fields — both pointer-wide, total size = 16 bytes with proper
// alignment. §9 / §4.4.
BEZEL_TEST(array_record_element, two_ref_fields_get_both_spec_9_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record StrPair(First: string, Second: string); "
        "class C public static method Main(): int "
        "  using var a := new Array<StrPair>(); "
        "  a.Add(new StrPair(\"hello\", \"world\")); "
        "  a.Add(new StrPair(\"foo\", \"bar\")); "
        "  var r0 := a.Get(0); var r1 := a.Get(1); "
        "  return int(r0.First = \"hello\" and r0.Second = \"world\" "
        "         and r1.First = \"foo\" and r1.Second = \"bar\"); "
        "end Main; end C;"), 1);
}
