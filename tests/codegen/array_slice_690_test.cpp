// Spec-derived tests for fix #690 — arr[range] typed as Array<T> per §15.6.
//
// Authority: §15.6 "Range slicing `arr[a..b]`, `arr[..b]`, `arr[a..]`,
// `arr[..]` is supported on Array<T>. Slicing produces a new Array<T>
// containing copies of the references (or values) in the range; mutating
// the slice does not affect the original."
//
// Pre-fix (from #682's own comment): sema typed `arr[range]` as the ELEMENT
// type T, so member access on the slice result (`s.Count`, `s[i]`,
// `for var x in s`) could not resolve — Count/iteration on the inferred-T
// local fell through or produced wrong layout.
//
// Post-fix: sema types `arr[range]` as Array<T> (the whole receiver type),
// so all the documented Array<T> operations work on the slice.
//
// These tests supplement the #682 tests (which only verified no-ICE / IR
// validity) with value-correctness assertions: Count, element access,
// slice-of-slice, and open-ended forms.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── §15.6 / #690: Count of the slice matches the range length ────────────────

// §15.6 / #690: arr[1..3] inclusive — Count is 3 (elements at indices 1,2,3).
// Pre-fix: Count returned 0 (was typed as int.Count → unsupported → 0).
BEZEL_TEST(array_slice_690, inclusive_slice_count) {
    // per §15.6
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); arr.Add(20); arr.Add(30); arr.Add(40); arr.Add(50); "
        "    using var s := arr[1..3]; "
        "    return s.Count; "   // elements at 1,2,3 → Count = 3
        "  end Main; "
        "end C;"), 3);
}

// §15.6 / #690: arr[0..4] inclusive — Count is 5 (all five elements).
BEZEL_TEST(array_slice_690, full_range_inclusive_count) {
    // per §15.6
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(1); arr.Add(2); arr.Add(3); arr.Add(4); arr.Add(5); "
        "    using var s := arr[0..4]; "
        "    return s.Count; "  // all 5 elements
        "  end Main; "
        "end C;"), 5);
}

// §15.6 / #690: arr[2..<5] half-open — Count is 3 (elements at 2,3,4).
BEZEL_TEST(array_slice_690, half_open_slice_count) {
    // per §15.6 — half-open form excludes upper endpoint
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(0); arr.Add(10); arr.Add(20); arr.Add(30); arr.Add(40); arr.Add(50); "
        "    using var s := arr[2..<5]; "
        "    return s.Count; "  // indices 2,3,4 → Count = 3
        "  end Main; "
        "end C;"), 3);
}

// ── §15.6 / #690: element values are correct ─────────────────────────────────

// §15.6 / #690: arr[1..3] inclusive — element at index 0 in the slice is
// the element at index 1 in the original array (value 20).
BEZEL_TEST(array_slice_690, inclusive_slice_first_element) {
    // per §15.6
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); arr.Add(20); arr.Add(30); arr.Add(40); arr.Add(50); "
        "    using var s := arr[1..3]; "
        "    return s[0]; "   // slice[0] = arr[1] = 20
        "  end Main; "
        "end C;"), 20);
}

// §15.6 / #690: arr[1..3] inclusive — element at index 2 in the slice is
// the element at index 3 in the original array (value 40).
BEZEL_TEST(array_slice_690, inclusive_slice_last_element) {
    // per §15.6
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); arr.Add(20); arr.Add(30); arr.Add(40); arr.Add(50); "
        "    using var s := arr[1..3]; "
        "    return s[2]; "   // slice[2] = arr[3] = 40
        "  end Main; "
        "end C;"), 40);
}

// §15.6 / #690: arr[2..<5] half-open — element at index 1 in the slice
// is the element at index 3 in the original (value 30).
BEZEL_TEST(array_slice_690, half_open_slice_middle_element) {
    // per §15.6
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(0); arr.Add(10); arr.Add(20); arr.Add(30); arr.Add(40); arr.Add(50); "
        "    using var s := arr[2..<5]; "
        "    return s[1]; "   // slice[1] = arr[3] = 30
        "  end Main; "
        "end C;"), 30);
}

// ── §15.6 / §16.2 / #690: open-ended slice forms ────────────────────────────

// §15.6 / §16.2 / #690: arr[2..] open-ended from 2 — Count is (5 - 2) = 3.
BEZEL_TEST(array_slice_690, open_upper_slice_count) {
    // per §15.6 / §16.2
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(1); arr.Add(2); arr.Add(3); arr.Add(4); arr.Add(5); "
        "    using var s := arr[2..]; "
        "    return s.Count; "  // indices 2,3,4 → Count = 3
        "  end Main; "
        "end C;"), 3);
}

// §15.6 / §16.2 / #690: arr[..2] inclusive open lower — Count is 3 (0,1,2).
BEZEL_TEST(array_slice_690, open_lower_inclusive_count) {
    // per §15.6 / §16.2
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); arr.Add(20); arr.Add(30); arr.Add(40); arr.Add(50); "
        "    using var s := arr[..2]; "
        "    return s.Count; "  // indices 0,1,2 → Count = 3
        "  end Main; "
        "end C;"), 3);
}

// §15.6 / §16.2 / #690: arr[..] full array — Count matches original.
BEZEL_TEST(array_slice_690, full_open_slice_count) {
    // per §15.6 / §16.2
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(1); arr.Add(2); arr.Add(3); "
        "    using var s := arr[..]; "
        "    return s.Count; "  // all 3 elements
        "  end Main; "
        "end C;"), 3);
}

// ── §15.6 / #690: slice is independent (mutation does not affect original) ──

// §15.6: "mutating the slice does not affect the original". Slice the array,
// add an element to the slice, verify original Count is unchanged.
BEZEL_TEST(array_slice_690, slice_is_independent_mutation) {
    // per §15.6 — "mutating the slice does not affect the original"
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(1); arr.Add(2); arr.Add(3); arr.Add(4); arr.Add(5); "
        "    using var s := arr[1..3]; "
        "    s.Add(99); "          // mutate the slice
        "    return arr.Count; "   // original unchanged → 5
        "  end Main; "
        "end C;"), 5);
}

// ── §15.6 / #690: for-in iteration over a slice ─────────────────────────────

// §15.6 / #690: iterating a slice with for-in yields the correct elements.
// Sum arr[1..3] = 20 + 30 + 40 = 90.
BEZEL_TEST(array_slice_690, for_in_slice_sum) {
    // per §15.6
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); arr.Add(20); arr.Add(30); arr.Add(40); arr.Add(50); "
        "    using var s := arr[1..3]; "
        "    var sum := 0; "
        "    for var x in s do sum := sum + x; end; "
        "    return sum; "  // 20 + 30 + 40 = 90
        "  end Main; "
        "end C;"), 90);
}

// ── §15.6 / #690: slice-of-slice ─────────────────────────────────────────────

// §15.6 / #690: a slice produces Array<T>, so it can itself be sliced.
// arr[1..4] gives [20,30,40,50]; s[0..1] gives [20,30]; Count = 2.
BEZEL_TEST(array_slice_690, slice_of_slice_count) {
    // per §15.6 — result is Array<T> so it supports all Array<T> ops
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); arr.Add(20); arr.Add(30); arr.Add(40); arr.Add(50); "
        "    using var s1 := arr[1..4]; "   // [20,30,40,50]
        "    using var s2 := s1[0..1]; "    // [20,30]
        "    return s2.Count; "             // 2
        "  end Main; "
        "end C;"), 2);
}

// §15.6 / #690: assign slice to an explicit Array<T> local — type must
// be compatible (pre-fix: typed as int, so 'not implicitly convertible').
BEZEL_TEST(array_slice_690, assign_slice_to_array_local) {
    // per §15.6 — the slice result is Array<int>
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(5); arr.Add(10); arr.Add(15); "
        "    using var sub: Array<int> := arr[0..1]; "
        "    return sub.Count; "  // 2 elements (0 and 1 inclusive)
        "  end Main; "
        "end C;"), 2);
}

// ── §15.6 / #682 regression: scalar indexer path unchanged ──────────────────

// §15.6 / #690 regression guard: scalar `arr[i]` (int index) must still
// return the element value (T), not Array<T>.
BEZEL_TEST(array_slice_690, regression_scalar_indexer_unchanged) {
    // per §15.6 / fix #690 regression — non-range index returns T
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(7); arr.Add(42); arr.Add(99); "
        "    return arr[1]; "  // element at index 1 = 42
        "  end Main; "
        "end C;"), 42);
}
