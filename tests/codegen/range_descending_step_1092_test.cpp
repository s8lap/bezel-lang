// Regression tests for #1092 (§16): Range<int>.IsEmpty and Contains/`in`
// ignored the step direction, so they were wrong for descending and
// direction-mismatch ranges:
//   - IsEmpty(10..0 step -1) returned true (the range is non-empty: 10,9,…,0)
//   - 10 in (10..0 step -1) returned false (10 is the inclusive Start)
//   - IsEmpty(0..10 step -1) returned false (direction disagrees with step
//     sign → empty iteration, §16 L2333)
// IsEmpty now branches on the step sign; Contains/`in` (v0.162, fix #1340)
// use nonempty-AND-[min(Start,End),max(Start,End)] — IsEmpty (direction-aware)
// gates membership, so a stepped descending range is nonempty and Contains
// works correctly.  Found by adversarial discovery sweep #6.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1092: a descending range with a matching negative step is NOT empty.
BEZEL_TEST(range_descending_step_1092, descending_range_not_empty) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 10..0 step -1; "
        "  return 1 if r.IsEmpty else 0; "                // 0 (non-empty)
        "end Main; end C;"), 0);
}

// #1092: a range whose direction disagrees with its step sign IS empty (§16).
BEZEL_TEST(range_descending_step_1092, direction_mismatch_is_empty) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 0..10 step -1; "
        "  return 1 if r.IsEmpty else 0; "                // 1 (empty)
        "end Main; end C;"), 1);
}

// #1092: Contains (via `in`) is Step-independent — the Start endpoint of a
// descending range is contained.
BEZEL_TEST(range_descending_step_1092, contains_start_of_descending) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 10..0 step -1; "
        "  return 1 if 10 in r else 0; "                  // 1 (Start, inclusive)
        "end Main; end C;"), 1);
}

// #1092: both endpoints and an interior value are contained in a closed
// descending range; an out-of-interval value is not.
BEZEL_TEST(range_descending_step_1092, contains_endpoints_and_interior) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 10..0 step -1; "
        "  var a := 1 if 0 in r else 0; "                 // End, inclusive → 1
        "  var b := 1 if 7 in r else 0; "                 // interior → 1
        "  var c := 1 if 11 in r else 0; "                // outside → 0
        "  return a + b * 2 + c * 4; "                    // 1 + 2 + 0 = 3
        "end Main; end C;"), 3);
}

// #1092: a HALF-OPEN descending range excludes the End endpoint specifically.
BEZEL_TEST(range_descending_step_1092, half_open_descending_excludes_end) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 10..<0 step -1; "
        "  var a := 1 if 10 in r else 0; "                // Start → 1
        "  var b := 1 if 0 in r else 0; "                 // End excluded → 0
        "  var c := 1 if 5 in r else 0; "                 // interior → 1
        "  return a + b * 2 + c * 4; "                    // 1 + 0 + 4 = 5
        "end Main; end C;"), 5);
}

// Regression guard: ascending ranges are unchanged.
BEZEL_TEST(range_descending_step_1092, ascending_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 0..10; "
        "  var a := 1 if 5 in r else 0; "                 // 1
        "  var b := 1 if r.IsEmpty else 0; "              // 0 (non-empty)
        "  var r2: Range<int> := 0..<10; "
        "  var c := 1 if 10 in r2 else 0; "               // half-open excl → 0
        "  return a + b * 2 + c * 4; "                    // 1 + 0 + 0 = 1
        "end Main; end C;"), 1);
}
