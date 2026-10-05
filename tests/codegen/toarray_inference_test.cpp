// Codegen tests for `.ToArray()` / `.ToSet()` without a type annotation on the
// receiving local — §14 local-type inference + §5 LINQ materialization.
//
// ROOT CAUSE UNDER TEST:
//   Before the fix, sema typed `a.Where(...).ToArray()` as a lazy-sentinel
//   TypeRef (no astNode, no declaration). When `var b := …` had NO annotation,
//   the inferred type for `b` was therefore the lazy sentinel, and downstream
//   `b.Get(0)` / `b.Length` failed: for an int element the layout was wrong
//   (wrong element-type → codegen read slot-0 as an int32 from the array header
//   → returned 0 instead of 5); for a record element a string field's pointer
//   was ICmp-compared as a value → SIGABRT.
//
// The fix makes sema return a CONCRETE Array<T> TypeRef (astNode = "Array") for
// ToArray() and Set<T> for ToSet(), so inferred locals carry the right type for
// downstream member access — matching the annotated path (which already works).
//
// Tests are derived from §14 (local-type inference) and §5 (LINQ materializers).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ===========================================================================
// §5 + §14: ToArray() on int element — un-annotated local (bug repro #2)
// Before the fix: returns 0 (element-type mismatch).
// After the fix: returns 5 (first element of filtered array).
// ===========================================================================

BEZEL_TEST(toarray_inference, int_elem_get0_spec_14_5) {
    // var b := a.Where(...).ToArray()  — no annotation
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(5); a.Add(2); a.Add(8); "
        "  using var b := a.Where(lambda(x) => x > 0).ToArray(); "
        "  return b.Get(0); "  // expected 5
        "end Main; end C;"), 5);
}

BEZEL_TEST(toarray_inference, int_elem_length_spec_14_5) {
    // var b := … — check that b.Length resolves with the inferred type
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  using var b := a.Where(lambda(x) => x > 2).ToArray(); "
        "  return b.Count; "  // 2 elements > 2: {3,4}
        "end Main; end C;"), 2);
}

// ===========================================================================
// §5 + §14: ToArray() on record element — un-annotated local (bug repro #1)
// Before the fix: SIGABRT (ICmp type mismatch on string-pointer comparison).
// After the fix: returns 1 (string equality holds).
// ===========================================================================

BEZEL_TEST(toarray_inference, record_elem_field_read_spec_14_5) {
    // record R(S: string, N: int)
    // var b := a.Where((x) => x.N > 0).ToArray()  — no annotation
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record R(S: string, N: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<R>(); "
        "  a.Add(new R(\"hello\", 1)); a.Add(new R(\"world\", 2)); "
        "  using var b := a.Where(lambda(x) => x.N > 0).ToArray(); "
        "  var t := b.Get(0); var s := t.S; "
        "  return int(s = \"hello\"); "  // expected 1
        "end Main; end C;"), 1);
}

BEZEL_TEST(toarray_inference, record_elem_length_spec_14_5) {
    // b.Length on inferred-type record array
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record R(X: int, Y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<R>(); "
        "  a.Add(new R(1, 2)); a.Add(new R(3, 4)); a.Add(new R(5, 6)); "
        "  using var b := a.Where(lambda(r) => r.X > 1).ToArray(); "
        "  return b.Count; "  // 2 elements: (3,4) and (5,6)
        "end Main; end C;"), 2);
}

// ===========================================================================
// §5 + §14: chains — Where().OrderBy().ToArray() — no annotation
// ===========================================================================

BEZEL_TEST(toarray_inference, chain_where_orderby_spec_14_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(5); a.Add(2); a.Add(8); a.Add(1); "
        "  using var b := a.Where(lambda(x) => x > 1).OrderBy(lambda(x: int) => x).ToArray(); "
        "  return b.Get(0); "  // sorted {2,5,8} → first = 2
        "end Main; end C;"), 2);
}

BEZEL_TEST(toarray_inference, chain_skip_take_spec_14_5) {
    // Skip().Take().ToArray() — no annotation on result
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); a.Add(40); a.Add(50); "
        "  using var b := a.Skip(1).Take(3).ToArray(); "
        "  return b.Get(0); "  // elements 20,30,40 → first = 20
        "end Main; end C;"), 20);
}

BEZEL_TEST(toarray_inference, chain_skip_take_length_spec_14_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); a.Add(40); a.Add(50); "
        "  using var b := a.Skip(1).Take(3).ToArray(); "
        "  return b.Count; "  // 3 elements: 20,30,40
        "end Main; end C;"), 3);
}

// ===========================================================================
// §5 + §14: for-in over ToArray() result with inferred type
// ===========================================================================

BEZEL_TEST(toarray_inference, for_in_inferred_array_spec_14_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  using var b := a.Where(lambda(x) => x mod 2 = 0).ToArray(); "
        "  var s := 0; "
        "  for var x in b do s := s + x; end; "
        "  return s; "  // 2 + 4 = 6
        "end Main; end C;"), 6);
}

// ===========================================================================
// CONTROL: annotated using var b: Array<int> := … still works after fix
// (regression guard — the fix must not break the annotated path)
// ===========================================================================

BEZEL_TEST(toarray_inference, control_annotated_still_works_spec_14_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(5); a.Add(2); a.Add(8); "
        "  using var b: Array<int> := a.Where(lambda(x) => x > 0).ToArray(); "
        "  return b.Get(0); "  // 5 — same as the un-annotated case
        "end Main; end C;"), 5);
}

// ===========================================================================
// CONTROL: direct a.ToArray() without a lazy op — no annotation
// ===========================================================================

BEZEL_TEST(toarray_inference, direct_toarray_no_annotation_spec_14_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(7); a.Add(3); "
        "  using var b := a.ToArray(); "
        "  return b.Get(0); "  // 7
        "end Main; end C;"), 7);
}

// ===========================================================================
// §5 + §14: string element — un-annotated ToArray
// ===========================================================================

BEZEL_TEST(toarray_inference, string_elem_spec_14_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"hello\"); a.Add(\"world\"); a.Add(\"hi\"); "
        "  using var b := a.Where(lambda(s) => s.Length > 3).ToArray(); "
        "  return b.Count; "  // "hello" and "world" — 2 elements
        "end Main; end C;"), 2);
}

BEZEL_TEST(toarray_inference, string_elem_get0_spec_14_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"hello\"); a.Add(\"world\"); "
        "  using var b := a.Where(lambda(s) => s.Length > 3).ToArray(); "
        "  return int(b.Get(0) = \"hello\"); "
        "end Main; end C;"), 1);
}
