// fix #789 / stdlib §5: the predicate overloads Last(pred)/Single(pred)/
// LastOrNull(pred)/SingleOrNull(pred) (added to the LINQ surface in v0.145).
// Previously a predicate argument on Last/Single/LastOrNull was silently
// accepted and mis-lowered to 0 (only First/FirstOrNull/SingleOrNull had a
// predicate codegen path). Each predicate form is now equivalent to the
// no-argument method applied after Where(predicate).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
const char* arr123 =
    "  using var arr: Array<int> := new Array<int>(3); "
    "  arr.Add(1); arr.Add(2); arr.Add(3); ";
}  // namespace

// stdlib §5/#789: Last(pred) returns the LAST element satisfying the predicate
// (the issue's repro: [1,2,3].Last(x => x < 3) == 2, was 0).
BEZEL_TEST(linq_last_single_predicate_789, last_predicate) {
    BEZEL_EXPECT_EQ(runFull(
        std::string("module M; class C public static method Main(): int ") +
        arr123 +
        "  return arr.Last(lambda(x) => x < 3); "  // 2
        "end Main; end C;"), 2);
}

// stdlib §5/#789: Single(pred) returns the sole element satisfying the predicate.
BEZEL_TEST(linq_last_single_predicate_789, single_predicate) {
    BEZEL_EXPECT_EQ(runFull(
        std::string("module M; class C public static method Main(): int ") +
        arr123 +
        "  return arr.Single(lambda(x) => x = 2); "  // 2
        "end Main; end C;"), 2);
}

// stdlib §5/#789: Single(pred) throws InvalidOperationException when more than
// one element matches.
BEZEL_TEST(linq_last_single_predicate_789, single_predicate_multiple_throws) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C public static method Main(): int "
        "  using var arr: Array<int> := new Array<int>(3); arr.Add(2); arr.Add(2); arr.Add(3); "
        "  try "
        "    var v: int := arr.Single(lambda(x) => x = 2); "
        "    return (7 if v = v else 8); "
        "  catch var ex: InvalidOperationException => do return 0; end "
        "  end; "
        "end Main; end C;"), 0);
}

// stdlib §5/#789: Single(pred) throws when NO element matches.
BEZEL_TEST(linq_last_single_predicate_789, single_predicate_none_throws) {
    BEZEL_EXPECT_EQ(runFull(
        std::string("module M; class C public static method Main(): int ") +
        arr123 +
        "  try "
        "    var v: int := arr.Single(lambda(x) => x = 9); "
        "    return (7 if v = v else 8); "
        "  catch var ex: InvalidOperationException => do return 0; end "
        "  end; "
        "end Main; end C;"), 0);
}

// stdlib §5/#789: Last(pred) throws when no element matches.
BEZEL_TEST(linq_last_single_predicate_789, last_predicate_none_throws) {
    BEZEL_EXPECT_EQ(runFull(
        std::string("module M; class C public static method Main(): int ") +
        arr123 +
        "  try "
        "    var v: int := arr.Last(lambda(x) => x > 9); "
        "    return (7 if v = v else 8); "
        "  catch var ex: InvalidOperationException => do return 0; end "
        "  end; "
        "end Main; end C;"), 0);
}

// stdlib §5/#789: LastOrNull(pred) returns the last match (as T?, here unwrapped).
BEZEL_TEST(linq_last_single_predicate_789, last_or_null_predicate_match) {
    BEZEL_EXPECT_EQ(runFull(
        std::string("module M; class C public static method Main(): int ") +
        arr123 +
        "  return (arr.LastOrNull(lambda(x) => x < 3) ?? -1); "  // 2
        "end Main; end C;"), 2);
}

// stdlib §5/#789: LastOrNull(pred) returns null when no element matches.
// -1 does not round-trip through the 8-bit process exit code (runAotOut: the
// value channel for negative/out-of-range results).
BEZEL_TEST(linq_last_single_predicate_789, last_or_null_predicate_none) {
    BEZEL_EXPECT_EQ(runAotOut(
        std::string("module M; class C public static method Main(): int ") +
        arr123 +
        "  Console.WriteLine(arr.LastOrNull(lambda(x) => x > 9) ?? -1); "  // null → -1
        "  return 0; "
        "end Main; end C;"), "-1");
}

// stdlib §5/#789: the predicate forms also work over a LAZY source (Range),
// exercising the pre-materialization path. 1..5 last < 4 == 3.
BEZEL_TEST(linq_last_single_predicate_789, last_predicate_over_range) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C public static method Main(): int "
        "  return (1..5).Last(lambda(x) => x < 4); "  // 3
        "end Main; end C;"), 3);
}

// stdlib §5: regression — First(pred) (already implemented) still returns the
// FIRST match, distinct from Last(pred), confirming the new code didn't disturb it.
BEZEL_TEST(linq_last_single_predicate_789, first_vs_last_distinct) {
    BEZEL_EXPECT_EQ(runFull(
        std::string("module M; class C public static method Main(): int ") +
        arr123 +
        "  return arr.First(lambda(x) => x < 3) * 10 + arr.Last(lambda(x) => x < 3); "  // 1*10+2 = 12
        "end Main; end C;"), 12);
}
