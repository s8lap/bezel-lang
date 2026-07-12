// Spec-derived tests for fix #746 (R41 cluster J) — OwningBuffer<T>.AsSpan /
// AsReadOnlySpan with a Range<int> argument honours the range's endpoint.
//
// Authority: stdlib §4.14 — AsSpan(r: Range<int>): Start = r.Start, Length =
// the range's element count (End - Start + 1 inclusive, End - Start half-open);
// an out-of-bounds endpoint raises IndexOutOfRangeException.
//
// Pre-fix the Range argument fell into the single-arg `AsSpan(start)` overload,
// which read only the low bound and set Length = Count - Start — ignoring the
// End (and never bounds-checking it). `AsSpan(2..5).Length` returned 8 (= 10-2)
// instead of 4.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §4.14: AsSpan(a..b) (inclusive) — Length = b - a + 1.
BEZEL_TEST(asspan_range_746, inclusive_range_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(10); "
        "  return buf.AsSpan(2..5).Length; "  // 4, not 8
        "end Main; end C;"), 4);
}

// §4.14: AsSpan(a..<b) (half-open) — Length = b - a.
BEZEL_TEST(asspan_range_746, half_open_range_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(10); "
        "  return buf.AsSpan(2..<5).Length; "  // 3
        "end Main; end C;"), 3);
}

// §4.14: a full inclusive range spans the whole buffer.
BEZEL_TEST(asspan_range_746, full_inclusive_range) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(10); "
        "  return buf.AsReadOnlySpan(0..9).Length; "  // 10
        "end Main; end C;"), 10);
}

// §4.14: an out-of-bounds endpoint raises IndexOutOfRangeException (does NOT
// silently clamp). Catch it to verify the throw (an unhandled throw would exit
// the in-process JIT runner).
BEZEL_TEST(asspan_range_746, out_of_bounds_end_throws) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(5); "
        "  try "
        "    var sp := buf.AsSpan(2..10); "  // end 10 > Count 5
        "    return 0; "                       // no throw → fail
        "  catch IndexOutOfRangeException => return 1; "  // caught → pass
        "  end; "
        "end Main; end C;"), 1);
}

// §4.14: the single-int AsSpan(start) overload still means [start, Count).
BEZEL_TEST(asspan_range_746, single_int_start_overload_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(10); "
        "  return buf.AsSpan(2).Length; "  // 8 = Count - start
        "end Main; end C;"), 8);
}
