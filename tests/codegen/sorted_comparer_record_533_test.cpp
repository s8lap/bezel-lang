// Codegen regression — #533: Array<Record>.Sorted(comparer) and
// Array<Record>.Sort(comparer) emit `ret void` into an i32-returning function
// when the element type T is a record (IR verification failure).
//
// Root cause (#472 gap): lowerLinqLambda built a funcRef with only ONE
// funcParam (the element TypeRef), so for the two-param comparer (T,T)->int
// the second param `b` got no type hint, body member accesses on `b` failed
// (fieldAddr returned null), lowerExpr returned null, and the lambda body
// emitted `ret void` instead of `ret i32`.
//
// Spec references:
//   stdlib §4.4 — Array<T>.Sort(comparer: (T,T)->int): unit  (in-place)
//   stdlib §4.4 — Array<T>.Sorted(comparer: (T,T)->int): owned Array<T>
//   §9.1 / §9.2 — positional record semantics, field access
//   §27.5 — lambda closures, captured variables
//
// Tests in this file:
//   1. Array<Point>.Sorted(comparer) sorts by X ascending; s[0].X correct;
//      original array unchanged.
//   2. In-place Array<Point>.Sort(comparer) mutates correctly.
//   3. Record with two int fields: sort descending by Y.
//   4. Record with a string field: comparer on string-length.
//   5. Controls: Array<int>.Sorted and Array<string>.Sorted still pass.
//   6. Control: Array<Point>.Reversed() still works.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool rejectedAtCompile(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}

}  // namespace

// ===========================================================================
// §4.4 / #533: Array<Point>.Sorted(comparer) — copy sorted by X ascending.
// ===========================================================================

// §4.4 / #533: Sorted((a,b)=>a.X-b.X) on [Point(3,1), Point(1,2), Point(2,0)]
// produces a copy sorted ascending by X: [Point(1,2), Point(2,0), Point(3,1)].
// Encode: s.Get(0).X * 100 + s.Get(1).X * 10 + s.Get(2).X = 1*100+2*10+3 = 123.
BEZEL_TEST(sorted_comparer_record_533, sorted_copy_by_x_ascending_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(3, 1)); "
        "  a.Add(new Point(1, 2)); "
        "  a.Add(new Point(2, 0)); "
        "  using var s := a.Sorted(lambda(p, q) => p.X - q.X); "
        "  return s.Get(0).X * 100 + s.Get(1).X * 10 + s.Get(2).X; "  // 123
        "end Main; end C;"), 123);
}

// §4.4 / #533: original array is unchanged after Sorted(comparer).
// a[0].X=3, a[1].X=1, a[2].X=2 → encode 3*100+1*10+2 = 312.
BEZEL_TEST(sorted_comparer_record_533, sorted_original_unchanged_spec_4_4) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(3, 1)); "
        "  a.Add(new Point(1, 2)); "
        "  a.Add(new Point(2, 0)); "
        "  using var s := a.Sorted(lambda(p, q) => p.X - q.X); "
        "  Console.WriteLine(a.Get(0).X * 100 + a.Get(1).X * 10 + a.Get(2).X); return 0; "  // 312
        "end Main; end C;"), "312");
}

// §4.4 / #533: Sorted(comparer) result has correct length.
BEZEL_TEST(sorted_comparer_record_533, sorted_result_length_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(3, 1)); "
        "  a.Add(new Point(1, 2)); "
        "  a.Add(new Point(2, 0)); "
        "  using var s := a.Sorted(lambda(p, q) => p.X - q.X); "
        "  return s.Count; "  // 3
        "end Main; end C;"), 3);
}

// §4.4 / #533: Sorted(comparer) with explicit type-annotated params still works.
// (Ensures the annotated path also gets correct two-param IR.)
BEZEL_TEST(sorted_comparer_record_533, sorted_annotated_params_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(3, 1)); "
        "  a.Add(new Point(1, 2)); "
        "  a.Add(new Point(2, 0)); "
        "  using var s := a.Sorted(lambda(p: Point, q: Point) => p.X - q.X); "
        "  return s.Get(0).X * 100 + s.Get(1).X * 10 + s.Get(2).X; "  // 123
        "end Main; end C;"), 123);
}

// ===========================================================================
// §4.4 / #533: Array<Point>.Sort(comparer) — in-place sort with record T.
// ===========================================================================

// §4.4 / #533: in-place Sort((a,b)=>a.X-b.X) on [Point(3,1), Point(1,2),
// Point(2,0)] mutates arr to ascending-X order.
// Encode: a.Get(0).X*100 + a.Get(1).X*10 + a.Get(2).X = 1*100+2*10+3 = 123.
BEZEL_TEST(sorted_comparer_record_533, sort_inplace_by_x_ascending_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(3, 1)); "
        "  a.Add(new Point(1, 2)); "
        "  a.Add(new Point(2, 0)); "
        "  a.Sort(lambda(p, q) => p.X - q.X); "
        "  return a.Get(0).X * 100 + a.Get(1).X * 10 + a.Get(2).X; "  // 123
        "end Main; end C;"), 123);
}

// §4.4 / #533: in-place Sort with a descending comparer on Y field.
// [Point(3,1), Point(1,2), Point(2,0)] sorted desc by Y: [Point(1,2),
// Point(3,1), Point(2,0)]. Encode Y values: 2*100+1*10+0 = 210.
BEZEL_TEST(sorted_comparer_record_533, sort_inplace_by_y_descending_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(3, 1)); "
        "  a.Add(new Point(1, 2)); "
        "  a.Add(new Point(2, 0)); "
        "  a.Sort(lambda(p, q) => q.Y - p.Y); "  // descending Y
        "  return a.Get(0).Y * 100 + a.Get(1).Y * 10 + a.Get(2).Y; "  // 210
        "end Main; end C;"), 210);
}

// §4.4 / #533: Sort(comparer) with explicit type-annotated params.
BEZEL_TEST(sorted_comparer_record_533, sort_inplace_annotated_params_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(3, 1)); "
        "  a.Add(new Point(1, 2)); "
        "  a.Add(new Point(2, 0)); "
        "  a.Sort(lambda(p: Point, q: Point) => p.X - q.X); "
        "  return a.Get(0).X * 100 + a.Get(1).X * 10 + a.Get(2).X; "  // 123
        "end Main; end C;"), 123);
}

// ===========================================================================
// §4.4 / #533: Sort/Sorted with a record having multiple fields.
// Demonstrate field independence.
// ===========================================================================

// §4.4 / #533: record with 3 fields; sort by third field.
// Record Pt3(A: int, B: int, C: int). Sort by C ascending.
// Input: [(9,0,3), (9,0,1), (9,0,2)]. Sorted by C: [(9,0,1),(9,0,2),(9,0,3)].
// Encode C values: 1*100+2*10+3 = 123.
BEZEL_TEST(sorted_comparer_record_533, sort_record_three_fields_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pt3(A: int, B: int, C: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Pt3>(); "
        "  a.Add(new Pt3(9, 0, 3)); "
        "  a.Add(new Pt3(9, 0, 1)); "
        "  a.Add(new Pt3(9, 0, 2)); "
        "  a.Sort(lambda(p, q) => p.C - q.C); "
        "  return a.Get(0).C * 100 + a.Get(1).C * 10 + a.Get(2).C; "  // 123
        "end Main; end C;"), 123);
}

// §4.4 / #533: Sorted descending by X on a 4-element array.
// Input: [5, 2, 8, 1] (as Point.X). Sorted desc by X: [8, 5, 2, 1].
// Encode: 8*1000+5*100+2*10+1 = 8521.
BEZEL_TEST(sorted_comparer_record_533, sorted_copy_four_elements_desc_spec_4_4) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(5, 0)); "
        "  a.Add(new Point(2, 0)); "
        "  a.Add(new Point(8, 0)); "
        "  a.Add(new Point(1, 0)); "
        "  using var s := a.Sorted(lambda(p, q) => q.X - p.X); "  // descending
        "  Console.WriteLine(s.Get(0).X * 1000 + s.Get(1).X * 100 + "
        "         s.Get(2).X * 10 + s.Get(3).X); return 0; "  // 8521
        "end Main; end C;"), "8521");
}

// ===========================================================================
// §4.4 / #533: Controls — primitive Array<T> with comparer still works.
// (Regression guard: the fix must not break working primitive cases.)
// ===========================================================================

// §4.4 / #533 control: Array<int>.Sorted((a,b)=>a-b) still produces correct
// sorted copy.  [3,1,2] → sorted → [1,2,3]; encode 1*49+2*7+3=66.
BEZEL_TEST(sorted_comparer_record_533, control_int_sorted_comparer_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(3); a.Add(1); a.Add(2); "
        "  using var s := a.Sorted(lambda(x, y) => x - y); "
        "  return s.Get(0) * 49 + s.Get(1) * 7 + s.Get(2); "  // 66
        "end Main; end C;"), 66);
}

// §4.4 / #533 control: Array<int>.Sort((a,b)=>a-b) still mutates correctly.
// [3,1,2] → sort → [1,2,3]; encode 1*49+2*7+3=66.
BEZEL_TEST(sorted_comparer_record_533, control_int_sort_comparer_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(3); a.Add(1); a.Add(2); "
        "  a.Sort(lambda(x, y) => x - y); "
        "  return a.Get(0) * 49 + a.Get(1) * 7 + a.Get(2); "  // 66
        "end Main; end C;"), 66);
}

// §4.4 / #533 control: Array<string>.Sorted(comparer) still works.
// Sort ["cc","a","bb"] by length ascending → ["a","bb","cc"].
// Encode lengths: 1*49+2*7+2=63.
BEZEL_TEST(sorted_comparer_record_533, control_string_sorted_comparer_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"cc\"); a.Add(\"a\"); a.Add(\"bb\"); "
        "  using var s := a.Sorted(lambda(x, y) => x.Length - y.Length); "
        "  return s.Get(0).Length * 49 + s.Get(1).Length * 7 + s.Get(2).Length; "
        // 1*49+2*7+2 = 49+14+2 = 65
        "end Main; end C;"), 65);
}

// §3 / #533 control: Array<Point>.Reversed() still works after the #533 fix.
// [Point(1,0), Point(2,0), Point(3,0)] reversed → [Point(3,0), Point(2,0),
// Point(1,0)]. Encode X values: 3*100+2*10+1 = 321.
BEZEL_TEST(sorted_comparer_record_533, control_record_reversed_spec_3) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(1, 0)); "
        "  a.Add(new Point(2, 0)); "
        "  a.Add(new Point(3, 0)); "
        "  using var r := a.Reversed(); "
        "  Console.WriteLine(r.Get(0).X * 100 + r.Get(1).X * 10 + r.Get(2).X); return 0; "  // 321
        "end Main; end C;"), "321");
}
