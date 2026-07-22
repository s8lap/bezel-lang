// Codegen tests for issue #1579 (v0.182) — stdlib §4.4 / spec §16.3:
// the named `Array<T>.Slice(r: Range<int>): owned Array<T>` method.
//
// stdlib §4.4 declares:
//   "// Range slicing (language spec §15.6): `arr[r]` — arr[a..b], arr[..b],
//    arr[a..], arr[..] — is the compiler-recognized equivalent of this
//    method (v0.182):
//    public method Slice(r: Range<int>): owned Array<T>;   // fresh array;
//        the range's values, or copies of the element borrows"
//
// Before this fix the NAMED method form did not exist at all ("no member
// `Slice`"); `arr[r]`, its compiler-recognized equivalent, already worked
// (fix #682/#1079). The fix shares codegen between the two forms via
// lowerArrayRangeSlice, so both must always agree exactly — several tests
// below cross-check `.Slice(r)` against `arr[r]` on the identical range.
//
// runJit returns the raw exit code.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
bool rejects(const std::string& prog) {
    auto r = runAot(prog);
    return !r.ran;  // a compile error → not run
}
const char* kFiveElemArray =
    "module M; class C public static method Main(): int "
    "  using var arr := new Array<int>(); "
    "  arr.Add(10); arr.Add(20); arr.Add(30); arr.Add(40); arr.Add(50); ";
}  // namespace

// stdlib §4.4: `.Slice(a..b)` (inclusive range) returns a fresh Array<T>
// with the expected Count and element values.
BEZEL_TEST(array_slice_named_method_1579, slice_closed_range_count_and_values) {
    BEZEL_EXPECT_EQ(run(std::string(kFiveElemArray) +
        "  using var s := arr.Slice(1..3); "        // indices 1,2,3 inclusive
        "  return s.Count; "
        "end Main; end C;"), 3);
}

BEZEL_TEST(array_slice_named_method_1579, slice_closed_range_first_element) {
    BEZEL_EXPECT_EQ(run(std::string(kFiveElemArray) +
        "  using var s := arr.Slice(1..3); "
        "  return s[0]; "                            // element at source index 1
        "end Main; end C;"), 20);
}

// §16.3 / spec §4.4: `.Slice(r)` and `arr[r]` are the "compiler-recognized
// equivalent" forms — they must produce Arrays of the same Count on the
// identical range.
BEZEL_TEST(array_slice_named_method_1579, slice_method_matches_indexer_count) {
    BEZEL_EXPECT_EQ(run(std::string(kFiveElemArray) +
        "  using var s1 := arr.Slice(1..3); "
        "  using var s2 := arr[1..3]; "
        "  return (if s1.Count = s2.Count then 1 else 0); "
        "end Main; end C;"), 1);
}

// Cross-check element-by-element, not just Count.
BEZEL_TEST(array_slice_named_method_1579, slice_method_matches_indexer_elements) {
    BEZEL_EXPECT_EQ(run(std::string(kFiveElemArray) +
        "  using var s1 := arr.Slice(1..3); "
        "  using var s2 := arr[1..3]; "
        "  return (if (s1[0] = s2[0]) and (s1[1] = s2[1]) and "
        "                (s1[2] = s2[2]) then 1 else 0); "
        "end Main; end C;"), 1);
}

// §16.2: an open-ended lower bound — `.Slice(..2)` slices from the start
// through index 2 inclusive → indices 0,1,2 → Count 3.
BEZEL_TEST(array_slice_named_method_1579, slice_open_lower_bound) {
    BEZEL_EXPECT_EQ(run(std::string(kFiveElemArray) +
        "  using var s := arr.Slice(..2); "
        "  return s.Count; "
        "end Main; end C;"), 3);
}

// §16.2: an open-ended upper bound — `.Slice(3..)` slices from index 3
// through the last element → indices 3,4 → Count 2.
BEZEL_TEST(array_slice_named_method_1579, slice_open_upper_bound) {
    BEZEL_EXPECT_EQ(run(std::string(kFiveElemArray) +
        "  using var s := arr.Slice(3..); "
        "  return s.Count; "
        "end Main; end C;"), 2);
}

// §16.1: a half-open range (`..<`) via the named method — indices 0,1 only.
BEZEL_TEST(array_slice_named_method_1579, slice_half_open_range) {
    BEZEL_EXPECT_EQ(run(std::string(kFiveElemArray) +
        "  using var s := arr.Slice(0..<2); "
        "  return s.Count; "
        "end Main; end C;"), 2);
}

// §15.6: an out-of-range slice raises IndexOutOfRangeException — the shared
// lowerArrayRangeSlice bounds-check fires identically to the indexer form.
BEZEL_TEST(array_slice_named_method_1579, slice_out_of_range_throws) {
    BEZEL_EXPECT_EQ(run(std::string(kFiveElemArray) +
        "  try "
        "    using var s := arr.Slice(5..10); "
        "    return 0; "
        "  catch "
        "    var e: IndexOutOfRangeException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// §4.5: `OwningArray<T>.Slice` is NOT exposed — its elements are uniquely
// owned and cannot be duplicated into a fresh array by a borrowing slice.
BEZEL_TEST(array_slice_named_method_1579, owning_array_slice_rejected) {
    BEZEL_EXPECT_TRUE(rejects(
        "module M; class Cell public V: int := 0; end Cell; "
        "class C public static method Main(): int "
        "  using var arr := new OwningArray<Cell>(); "
        "  using var s := arr.Slice(0..1); "
        "  return 0; "
        "end Main; end C;"));
}

// A `var r := 1..3;` Range-typed VARIABLE argument (not a syntactic range
// literal) works identically — Slice shares the same Range-variable
// extraction lowerArrayRangeSlice inherited from the #1079 indexer fix.
BEZEL_TEST(array_slice_named_method_1579, slice_range_variable_argument) {
    BEZEL_EXPECT_EQ(run(std::string(kFiveElemArray) +
        "  var r := 1..3; "
        "  using var s := arr.Slice(r); "
        "  return s.Count; "
        "end Main; end C;"), 3);
}
