// fix #775 / §16.3: a `Range<T>` works as an IEnumerable<T> in the lazy LINQ
// pipeline — both as a `SelectMany` selector return (flatten) and in receiver
// position (Count/Sum/Where/Select/…). Previously a SelectMany Range selector
// SIGSEGV'd (the 32-byte Range struct return was called via a pointer-return
// trampoline ABI) and a Range receiver silently returned 0.
//
// The runtime gained a BZ_LAZY_RANGE source (bz_lazy_from_range) whose movenext
// mirrors the for-in counted-loop semantics: inclusive (a..b) / half-open
// (a..<b) per the flags byte, direction by the step sign. Codegen wraps a Range
// receiver/selector via bz_lazy_from_range (the SelectMany trampoline calls the
// selector through the struct-return ABI). Open-ended ranges raise
// InvalidOperationException (§16.3). Ranges are INCLUSIVE by default, so the
// flatten of [1,3] via (n)=>1..(n+1) is {1,2} ∪ {1,2,3,4} = 6 elements.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §16.3: the #775 repro — flatten an inclusive range per source element. Was
// SIGSEGV; now returns the correct inclusive count (6, NOT the issue's "7").
BEZEL_TEST(selectmany_range_selector_775, flatten_inclusive_count) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C public static method Main(): int "
        "  using var arr: Array<int> := new Array<int>(2); "
        "  arr.Add(1); arr.Add(3); "
        "  var total: int := arr.SelectMany((n) => (1..n + 1)).Count(); "
        "  return total mod 100; "  // {1,2} ∪ {1,2,3,4} = 6
        "end Main; end C;"), 6);
}

// §16.3: the flattened VALUES flow through (not just the count). For [1,3] with
// (n)=>1..n: 1..1={1}(=1) ∪ 1..3={1,2,3}(=6); sum = 7.
BEZEL_TEST(selectmany_range_selector_775, flatten_value_flow_sum) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C public static method Main(): int "
        "  using var arr: Array<int> := new Array<int>(2); "
        "  arr.Add(1); arr.Add(3); "
        "  return arr.SelectMany((n) => (1..n)).Sum(); "  // 1 + 6 = 7
        "end Main; end C;"), 7);
}

// §16.3: a half-open selector range `1..<n`. [2,3] → 1..<2={1}(1) ∪ 1..<3={1,2}(2)
// = 3 elements.
BEZEL_TEST(selectmany_range_selector_775, flatten_half_open) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C public static method Main(): int "
        "  using var arr: Array<int> := new Array<int>(2); "
        "  arr.Add(2); arr.Add(3); "
        "  return arr.SelectMany((n) => (1..<n)).Count(); "  // 3
        "end Main; end C;"), 3);
}

// §16.3: receiver-position aggregates — Count (inclusive) and Sum.
BEZEL_TEST(selectmany_range_selector_775, receiver_count_and_sum) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C public static method Main(): int "
        "  return (1..5).Count() + (1..5).Sum(); "  // 5 + 15 = 20
        "end Main; end C;"), 20);
}

// §16.3: a receiver-position lazy chain over a range (Where → Select → Sum).
// 1..10 evens = {2,4,6,8,10}; *1; sum = 30.
BEZEL_TEST(selectmany_range_selector_775, receiver_where_select_sum) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C public static method Main(): int "
        "  return (1..10).Where((x) => x mod 2 = 0).Select((x) => x).Sum(); "
        "end Main; end C;"), 30);
}

// §16.4: descending (step -1) and positive step counted correctly via the range
// source. 5..1 step -1 = {5,4,3,2,1} = 5; 1..10 step 2 = {1,3,5,7,9} = 5.
BEZEL_TEST(selectmany_range_selector_775, step_and_descending) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C public static method Main(): int "
        "  return (5..1 step -1).Count() + (1..10 step 2).Count(); "  // 10
        "end Main; end C;"), 10);
}

// §16.3 / stdlib §5: a LINQ lambda over a non-int Range element (Range<long>)
// must seed its untyped param with the element type. `(1L..3L).Select((x)=>x*2L)`
// previously left `x` untyped so codegen compiled the selector with an i32 param
// while the body produced i64 — an IR mismatch ("ret i64 … declared i32"). The
// fix seeds the Range element type into the LINQ lambda param (linqLambdaElementType).
BEZEL_TEST(selectmany_range_selector_775, typed_element_select_lambda) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C public static method Main(): int "
        "  return (0 if (1L..3L).Select((x) => x * 2L).Sum() = 12L else 1); "  // 2+4+6=12
        "end Main; end C;"), 0);
}

// §16.3 / stdlib §5: `Reverse()` on a Range source. The lazy `Reverse` operator
// was gated on a lazy receiver only (to avoid intercepting `Array.Reverse()`'s
// in-place form); a Range — which has no in-place Reverse — fell through to the
// Array in-place handler, so `(1..n).Reverse().Count()/.Sum()` returned 0 and
// `.ToArray()` SIGABRT'd (the bz.range struct was read as an array). The fix
// admits a Range receiver into the lazy Reverse path.
BEZEL_TEST(selectmany_range_selector_775, reverse_over_range) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C public static method Main(): int "
        "  return (1..5).Reverse().First() + (1..5).Reverse().Count(); "  // 5 + 5 = 10
        "end Main; end C;"), 10);
}

// §16.3: GetEnumerator on an open-ended range raises InvalidOperationException.
BEZEL_TEST(selectmany_range_selector_775, open_ended_raises) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C public static method Main(): int "
        "  var lo: int := 1; "
        "  try "
        "    return (lo..).Count(); "
        "  catch "
        "    var ex: InvalidOperationException => do return 5; end "
        "  end; "
        "end Main; end C;"), 5);
}
