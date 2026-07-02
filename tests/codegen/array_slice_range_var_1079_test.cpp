// Regression tests for #1079 (§15.6): slicing Array<T> with a range-typed
// VARIABLE (`var r := 0..2; arr[r]`) rather than a syntactic range literal.
// The Array<T> indexer type inference only recognised a syntactic ast::RangeExpr
// as a slice, so `arr[r]` (r: Range<int>) was mistyped as the element T (int)
// instead of Array<T>. Result: `.Count` read garbage and `for var x in arr[r]`
// failed at codegen ("not yet implemented: codegen for this for statement").
// The string-slice path already handled a range-typed variable; the fix mirrors
// it in the Array branch. The codegen indexer-get already lowered the
// Range-variable subscript via bz_array_slice — only the sema type was missing.
//
// runJit returns the raw exit code (0..255).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
bool rejects(const std::string& prog) {
    auto r = runJit(prog);
    return !r.ran;  // a compile error → not run
}
const char* kHead =
    "module M; class C public static method Main(): int "
    "  using var arr := new Array<int>(); "
    "  arr.Add(10); arr.Add(20); arr.Add(30); ";
}  // namespace

// A closed range variable slices Array<T> → Array<T>; .Count is correct.
BEZEL_TEST(array_slice_range_var_1079, closed_range_var_count) {
    BEZEL_EXPECT_EQ(run(std::string(kHead) +
        "  var r := 0..2; "
        "  using var s := arr[r]; "
        "  return s.Count; "                  // indices 0,1,2 → 3
        "end Main; end C;"), 3);
}

// The literal form keeps working (was already correct).
BEZEL_TEST(array_slice_range_var_1079, literal_range_count_unchanged) {
    BEZEL_EXPECT_EQ(run(std::string(kHead) +
        "  using var s := arr[0..2]; "
        "  return s.Count; "
        "end Main; end C;"), 3);
}

// A half-open range variable (`0..<2`) slices indices 0,1 → Count 2.
BEZEL_TEST(array_slice_range_var_1079, half_open_range_var_count) {
    BEZEL_EXPECT_EQ(run(std::string(kHead) +
        "  var r := 0..<2; "
        "  using var s := arr[r]; "
        "  return s.Count; "
        "end Main; end C;"), 2);
}

// for-in over a range-variable slice iterates the sliced elements.
BEZEL_TEST(array_slice_range_var_1079, for_in_over_range_var_slice) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var arr := new Array<int>(); "
        "  arr.Add(1); arr.Add(2); arr.Add(3); "
        "  var r := 0..2; "
        "  var sum := 0; "
        "  for var x in arr[r] do sum := sum + x; end; "
        "  return sum; "                       // 1+2+3 = 6
        "end Main; end C;"), 6);
}

// A scalar index variable still returns the element (not the array).
BEZEL_TEST(array_slice_range_var_1079, scalar_index_var_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var arr := new Array<int>(); "
        "  arr.Add(7); arr.Add(8); "
        "  var i := 1; "
        "  return arr[i]; "                     // element at 1 = 8
        "end Main; end C;"), 8);
}

// §15.6: slicing OwningArray<T> by a range VARIABLE is rejected, just like the
// literal form — the copy-returning slice would manufacture a second owner.
BEZEL_TEST(array_slice_range_var_1079, owned_array_range_var_rejected) {
    BEZEL_EXPECT_TRUE(rejects(
        "module M; class Cell public V: int := 0; end Cell; "
        "class C public static method Main(): int "
        "  using var arr := new OwningArray<Cell>(); "
        "  var r := 0..1; "
        "  using var s := arr[r]; "
        "  return 0; "
        "end Main; end C;"));
}
