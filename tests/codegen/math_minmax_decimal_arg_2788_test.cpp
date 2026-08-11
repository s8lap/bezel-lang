// Regression tests for #2788 — `Math.Min` / `Math.Max` / `Math.Clamp` with
// `decimal` arguments.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §2.2 ("`Math`", v0.240) declares
//     public static method Min<T>(a: T, b: T): T where T: IComparable<T>;
// and `decimal` (spec §4.4, stdlib §4.4) is a numeric primitive with a total
// order, so this is admissible and must return the lesser value.
//
// THE DEFECT WAS A COMPILER ABORT.  decimal lowers to a STRUCT, not an LLVM
// integer or float, so it fell past every arm into the float fallback and
// `CreateSIToFP` fired:
//     SIToFP source must be integer or integer vector
// MEASURED on a944fcd.  This was the FOURTH category this family was found to be
// missing, after Interface (#2782), `string` (#2785) and the builtin interfaces
// (#2786) — each found by a separate defect report, which is why #2786's guard
// is category-blind rather than another enumeration.
//
// THE FIX reuses the runtime comparison every other decimal ordering already
// uses: `bz_decimal_cmp` over two struct slots, returning -1/0/+1 — the same
// path a plain `x < 2m` takes (LlvmBackend_Exprs2.cpp:7431's `decCmp`).  So
// `Math.Min` and the `<` operator agree by construction; equal_by_value_scale
// below pins the consequence that matters most (bz_decimal_cmp is
// scale-normalizing, so 1.50m and 1.5m compare equal).
//
// Ordering relative to #2786's guard: the decimal arm is tried BEFORE the
// fallback, so this fix converts that issue's stopgap rejection into the correct
// answer.  #2786's test deliberately does not pin decimal's rejection, precisely
// so this change does not read as a regression there.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
// Plain runAot throughout: decimal is a value type and none of these programs
// allocates, so generated-code ASan has nothing to observe here.  (The memory
// hazard in this family lives in the pointer arms — interface and string —
// which are covered under runAotChecked in the #2782 and #2785 tests.)
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

std::string main_(const std::string& body) {
    return "module M; class C  public static method Main(): int " + body +
           "  end Main; end C; ";
}
}  // namespace

// #2788 / stdlib §2.2 + §4.4 — the reported repro, which ABORTED the compiler.
// Min(2.5m, 1.5m) is 1.5m and Max is 2.5m; both are asserted by VALUE rather
// than via a length or count, so a wrong selection cannot pass.
BEZEL_TEST(math_minmax_decimal_arg_2788, min_max_decimal_arguments) {
    BEZEL_EXPECT_EQ(run(main_(
        "    var a: decimal := 2.5m; "
        "    var b: decimal := 1.5m; "
        "    var m := Math.Min(a, b); "
        "    var x := Math.Max(a, b); "
        "    if m = 1.5m and x = 2.5m then return 71; end if; "
        "    return 72; ")), 71);
}

// NEGATIVE operands — a comparison that got the sign wrong (or compared
// magnitudes) would answer 88 here.
BEZEL_TEST(math_minmax_decimal_arg_2788, negative_decimal_operands) {
    BEZEL_EXPECT_EQ(run(main_(
        "    var a: decimal := -3.25m; "
        "    var b: decimal := 2.0m; "
        "    if Math.Min(a, b) = -3.25m and Math.Max(a, b) = 2.0m "
        "      then return 87; end if; "
        "    return 88; ")), 87);
}

// SCALE NORMALIZATION.  `bz_decimal_cmp` compares by value, not by
// representation, so 1.50m and 1.5m are equal and either may be returned — what
// must hold is that the result equals 1.5m.  This is the case that would fail if
// the arm ever compared the raw struct bits instead of calling the runtime.
BEZEL_TEST(math_minmax_decimal_arg_2788, equal_by_value_different_scale) {
    BEZEL_EXPECT_EQ(run(main_(
        "    var a: decimal := 1.50m; "
        "    var b: decimal := 1.5m; "
        "    var m := Math.Min(a, b); "
        "    if m = 1.5m then return 85; end if; "
        "    return 86; ")), 85);
}

// `Clamp` reaches the comparator from its own call site, twice — below the low
// bound...
BEZEL_TEST(math_minmax_decimal_arg_2788, clamp_decimal_below_low_bound) {
    BEZEL_EXPECT_EQ(run(main_(
        "    var v: decimal := 0.5m; "
        "    var lo: decimal := 1.0m; "
        "    var hi: decimal := 9.0m; "
        "    var r := Math.Clamp(v, lo, hi); "
        "    if r = 1.0m then return 81; end if; "
        "    return 82; ")), 81);
}

// ...and within the range, where the value is returned unchanged.
BEZEL_TEST(math_minmax_decimal_arg_2788, clamp_decimal_within_range) {
    BEZEL_EXPECT_EQ(run(main_(
        "    var v: decimal := 5.0m; "
        "    var lo: decimal := 1.0m; "
        "    var hi: decimal := 9.0m; "
        "    var r := Math.Clamp(v, lo, hi); "
        "    if r = 5.0m then return 83; end if; "
        "    return 84; ")), 83);
}

// Agreement with the `<` operator, which lowers through the SAME
// `bz_decimal_cmp`.  Pinned rather than asserted in a comment: if the arm ever
// stops using the runtime comparison, Math.Min and `<` can disagree and this
// case catches it.
BEZEL_TEST(math_minmax_decimal_arg_2788, agrees_with_relational_operator) {
    BEZEL_EXPECT_EQ(run(main_(
        "    var a: decimal := 2.5m; "
        "    var b: decimal := 1.5m; "
        "    var viaMin := Math.Min(a, b); "
        "    var viaOp: decimal := (if a < b then a else b); "
        "    if viaMin = viaOp then return 11; end if; "
        "    return 22; ")), 11);
}

// CONTROL — the integer arm, which is tried before the decimal arm.
BEZEL_TEST(math_minmax_decimal_arg_2788, control_int_unchanged) {
    BEZEL_EXPECT_EQ(run(main_(
        "    return Math.Min(7, 3) * 10 + Math.Max(7, 3); ")), 37);
}

// CONTROL — the double arm.  `double` and `decimal` are distinct types with
// distinct lowerings, and the float fallback must still handle double.
BEZEL_TEST(math_minmax_decimal_arg_2788, control_double_unchanged) {
    BEZEL_EXPECT_EQ(run(main_(
        "    var a: double := 2.5; "
        "    var b: double := 1.5; "
        "    var m := Math.Min(a, b); "
        "    if m < 2.0 then return 15; end if; "
        "    return 25; ")), 15);
}
