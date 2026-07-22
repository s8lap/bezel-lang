// Regression tests for #1089 (§20 / §7.4): a conditional expression with a bare
// `null` branch (`T if c else null`) must be typed `T?` (nullable). typeConditional
// treated the `null` branch as a bottom/diverging branch (a `null` literal carries
// no type), so the result was typed as the non-null branch's type `T`. An inferred
// `var x := T if c else null` was then non-nullable → x got null-narrowed (§7.4) →
// `x ?? d` lowered to a bare value-read (ignoring the null tag) and returned T's
// zero on the null path instead of `d`. The fix types a `null`-branch conditional
// as the other branch's type made nullable.
//
// runJit returns the FULL int from Main(); values ≤ 255.

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

// #1089: the null path of an inferred `T if c else null` is null, so `?? d`
// returns d.
BEZEL_TEST(conditional_null_branch_nullable_1089, null_path_coalesces_to_fallback) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := (if (1 > 2) then 42 else null); "        // cond false → null
        "  return x ?? 99; "                            // 99, was 0
        "end Main; end C;"), 99);
}

// The non-null path holds the value.
BEZEL_TEST(conditional_null_branch_nullable_1089, value_path_holds_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := (if (2 > 1) then 42 else null); "        // cond true → 42
        "  return x ?? 99; "                            // 42
        "end Main; end C;"), 42);
}

// `= null` agrees with `??` (both see the value as null on the null path).
BEZEL_TEST(conditional_null_branch_nullable_1089, null_path_equals_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := (if (1 > 2) then 42 else null); "
        "  return (if x = null then 7 else 3); "               // 7 (x is null)
        "end Main; end C;"), 7);
}

// The `null`-then mirror (`null if c else T`) is nullable too.
BEZEL_TEST(conditional_null_branch_nullable_1089, null_then_branch_mirror) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := (if (1 > 2) then null else 5); "          // cond false → 5
        "  return x ?? 99; "                            // 5
        "end Main; end C;"), 5);
}

// A nullable REFERENCE conditional (`Cell if c else null`) — null path is null.
BEZEL_TEST(conditional_null_branch_nullable_1089, nullable_reference_null_path) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Cell public V: int := 5; end Cell; "
        "class C public static method Main(): int "
        "  var c := new Cell(); "
        "  var x := (if (1 > 2) then c else null); "          // cond false → null
        "  return (if x = null then 7 else x!.V); "            // 7
        "end Main; end C;"), 7);
}

// A plain conditional with no null branch is unaffected.
BEZEL_TEST(conditional_null_branch_nullable_1089, plain_conditional_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := (if (1 > 2) then 5 else 7); "
        "  return x; "                                  // 7
        "end Main; end C;"), 7);
}
