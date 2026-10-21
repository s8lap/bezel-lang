// #6853 (spec §17.5, §5.1, §16.3, v0.340; stdlib §9.8): `x in r` IS the call
// `r.Contains(x)` over `Contains(value: T)`, so the operand is admitted exactly
// when it converts to the range's element type `T` under §5.1. §5.1 lists
// `int -> uint`/`uint -> int` among the conversions that are "never implicit".
//
// MEASURED on the pre-fix base (`b470e397`): `u in -5 .. 10` with `u: uint`
// compiled and printed False, and `(-5 .. 10).Contains(u)` compiled and printed
// True — the operand was not checked against `T` for a Range receiver at all.
// A `double` operand died in IR verification (`sext double`).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {
bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

std::string prog(const std::string& body) {
    return "module Conf;\n"
           "public class Program\n"
           "  public static method Main()\n" + body +
           "  end Main;\n"
           "end Program;\n";
}

const char* kInSite = "left operand of `in` over a `Range<T>` has a type that is "
                      "not implicitly convertible to the target type (\xC2\xA7""5.1)";
const char* kCallSite = "argument to `Range<T>.Contains` has a type that is not "
                        "implicitly convertible to the target type (\xC2\xA7""5.1)";
}  // namespace

// The issue's repro 2, verbatim.
BEZEL_TEST(range_in_contains_operand_conversion_6853, issue_repro_uint_in_int_range_literal_spec_5_1) {
    auto r = runAot(prog(
        "    var u: uint := 4294967295;\n"
        "    Console.WriteLine(u in -5 .. 10);\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kInSite));
}

BEZEL_TEST(range_in_contains_operand_conversion_6853, uint_in_stored_int_range_spec_5_1) {
    auto r = runAot(prog(
        "    var u: uint := 3;\n"
        "    var r := -5 .. 10;\n"
        "    Console.WriteLine(u in r);\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kInSite));
}

BEZEL_TEST(range_in_contains_operand_conversion_6853, uint_contains_call_spec_5_1) {
    auto r = runAot(prog(
        "    var u: uint := 3;\n"
        "    var r := -5 .. 10;\n"
        "    Console.WriteLine(r.Contains(u));\n"
        "    Console.WriteLine((-5 .. 10).Contains(u));\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kCallSite));
}

// fix-forward #6853 (§17.5, §16.6, §19.3, v0.340; spec-question #6897):
// against a range LITERAL whose endpoints are all constants, a `long` /
// `ulong` operand is ADMITTED when every endpoint fits its type — the literal
// adapts to the operand as `case n of 0 .. 10` does over a `long` (§16.6: a
// range pattern "agrees exactly with `in`"). Membership is decided on the
// mathematical values: boundaries, values outside, negative values, and a
// `long`/`ulong` value beyond `int`'s range. This row pinned a REJECTION on
// `d6a86161`; the base `b470e397` accepted these programs with these answers.
BEZEL_TEST(range_in_contains_operand_conversion_6853, long_and_ulong_into_int_range_spec_5_1) {
    auto r1 = runAot(prog(
        "    var l: long := 3;\n"
        "    Console.WriteLine(l in 0 .. 10);\n"
        "    Console.WriteLine((0 .. 10).Contains(l));\n"
        "    l := 0;\n"
        "    Console.WriteLine($\"{l in 0 .. 10} {l in 0 ..< 10} {(0 .. 10).Contains(l)}\");\n"
        "    l := 10;\n"
        "    Console.WriteLine($\"{l in 0 .. 10} {l in 0 ..< 10} {(0 ..< 10).Contains(l)}\");\n"
        "    l := -1;\n"
        "    Console.WriteLine($\"{l in 0 .. 10} {l in -5 .. 10} {(-5 .. 10).Contains(l)} {l in ..< 0} {l in 31 ..}\");\n"
        "    l := 5000000000;\n"
        "    Console.WriteLine($\"{l in 0 .. 10} {(0 .. 10).Contains(l)} {l in 31 ..} {(31 ..).Contains(l)}\");\n"
        "    l := -5000000000;\n"
        "    Console.WriteLine($\"{l in -5 .. 10} {l in ..< 0} {(..< 0).Contains(l)}\");\n"));
    BEZEL_EXPECT_TRUE(r1.ran);
    BEZEL_EXPECT_EQ(r1.stdoutText,
        "True\nTrue\n"
        "True True True\n"
        "True False False\n"
        "False True True True False\n"
        "False False True True\n"
        "False True True\n");
    auto r2 = runAot(prog(
        "    var g: ulong := 3;\n"
        "    Console.WriteLine((0 .. 10).Contains(g));\n"
        "    Console.WriteLine(g in 0 .. 10);\n"
        "    g := 11;\n"
        "    Console.WriteLine($\"{g in 0 .. 10} {(0 .. 10).Contains(g)}\");\n"
        "    g := 18446744073709551615UL;\n"
        "    Console.WriteLine($\"{g in 0 .. 10} {(0 .. 10).Contains(g)} {g in 31 ..} {(31 ..).Contains(g)} {g in ..< 5}\");\n"));
    BEZEL_EXPECT_TRUE(r2.ran);
    BEZEL_EXPECT_EQ(r2.stdoutText,
        "True\nTrue\n"
        "False False\n"
        "False False True True False\n");
}

// The range VALUE forms stay rejected: a variable's, a field's, a parameter's
// or a call result's static type `Range<int>` fixes `T`, and `long` / `ulong`
// do not convert to `int` (§5.1). Only a range LITERAL adapts (#6897).
BEZEL_TEST(range_in_contains_operand_conversion_6853, long_and_ulong_into_int_range_value_rejected_spec_5_1) {
    auto r1 = runAot(prog(
        "    var r := 0 .. 10;\n"
        "    var l: long := 3;\n"
        "    Console.WriteLine(l in r);\n"));
    BEZEL_EXPECT_FALSE(r1.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r1, kInSite));
    auto r2 = runAot(prog(
        "    var r := 0 .. 10;\n"
        "    var g: ulong := 3;\n"
        "    Console.WriteLine(r.Contains(g));\n"));
    BEZEL_EXPECT_FALSE(r2.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r2, kCallSite));
    auto r3 = runAot(
        "module Conf;\n"
        "public class Program\n"
        "  private static Window: Range<int> := 0 .. 10;\n"
        "  public static method Make(): Range<int>\n"
        "    return 0 .. 10;\n"
        "  end Make;\n"
        "  public static method Test(r: Range<int>, l: long): bool\n"
        "    return l in r;\n"
        "  end Test;\n"
        "  public static method Main()\n"
        "    var l: long := 3;\n"
        "    Console.WriteLine(l in Window);\n"
        "    Console.WriteLine(Make().Contains(l));\n"
        "    Console.WriteLine(Test(0 .. 10, l));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r3.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r3, kInSite));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r3, kCallSite));
}

// A range LITERAL with a NON-constant endpoint is admitted only when every
// endpoint's static type widens to the operand's (§5.1, #6897 rule W): `int`
// does not widen to `uint`, so `u in lo .. 10` with `lo: int` stays rejected
// at `in` and at `Contains`.
BEZEL_TEST(range_in_contains_operand_conversion_6853, uint_against_variable_int_endpoint_literal_rejected_spec_5_1) {
    auto r1 = runAot(prog(
        "    var lo: int := 0;\n"
        "    var u: uint := 3;\n"
        "    Console.WriteLine(u in lo .. 10);\n"));
    BEZEL_EXPECT_FALSE(r1.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r1, kInSite));
    auto r2 = runAot(prog(
        "    var lo: int := 0;\n"
        "    var u: uint := 3;\n"
        "    Console.WriteLine((lo .. 10).Contains(u));\n"));
    BEZEL_EXPECT_FALSE(r2.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r2, kCallSite));
}

// A `double` operand: pre-fix this died in LLVM IR verification.
BEZEL_TEST(range_in_contains_operand_conversion_6853, double_operand_rejected_at_sema_spec_5_1) {
    auto r = runAot(prog(
        "    Console.WriteLine(2.5 in 1 .. 5);\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kInSite));
}

// fix-forward #6853 (§17.5, §16.6, v0.340; spec-question #6897): an `int`
// operand against the constant literal `1U .. 10U` is ADMITTED — both
// endpoints fit `int` — and a negative operand is simply outside. This row
// pinned a REJECTION on `d6a86161`; the base accepted it with these answers.
BEZEL_TEST(range_in_contains_operand_conversion_6853, int_into_uint_range_spec_5_1) {
    auto r = runAot(prog(
        "    var i: int := 3;\n"
        "    Console.WriteLine(i in 1U .. 10U);\n"
        "    Console.WriteLine((1U .. 10U).Contains(i));\n"
        "    i := -1;\n"
        "    Console.WriteLine($\"{i in 1U .. 10U} {(1U .. 10U).Contains(i)} {i in 5U ..}\");\n"
        "    i := 10;\n"
        "    Console.WriteLine($\"{i in 1U .. 10U} {i in 1U ..< 10U} {(1U ..< 10U).Contains(i)}\");\n"
        "    i := -2147483648;\n"
        "    Console.WriteLine($\"{i in 0U .. 10U} {i in 5U ..}\");\n"
        "    i := 2147483647;\n"
        "    Console.WriteLine($\"{i in 0U .. 10U} {i in 5U ..} {(5U ..).Contains(i)}\");\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "True\nTrue\n"
        "False False False\n"
        "True False False\n"
        "False False\n"
        "False True True\n");
}

// The range VALUE form stays rejected: `Range<uint>` fixes `T`, and §5.1 makes
// `int -> uint` never implicit. So does a literal with an endpoint the operand
// cannot hold (`-5` fits no `uint`), the issue's repro 2 shape at `Contains`.
BEZEL_TEST(range_in_contains_operand_conversion_6853, int_into_uint_range_value_rejected_spec_5_1) {
    auto r1 = runAot(prog(
        "    var r := 1U .. 10U;\n"
        "    var i: int := 3;\n"
        "    Console.WriteLine(i in r);\n"));
    BEZEL_EXPECT_FALSE(r1.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r1, kInSite));
    auto r2 = runAot(prog(
        "    var r := 1U .. 10U;\n"
        "    var i: int := 3;\n"
        "    Console.WriteLine(r.Contains(i));\n"));
    BEZEL_EXPECT_FALSE(r2.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r2, kCallSite));
    auto r3 = runAot(prog(
        "    var u: uint := 3;\n"
        "    Console.WriteLine((-5 .. 10).Contains(u));\n"));
    BEZEL_EXPECT_FALSE(r3.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r3, kCallSite));
}

// CONTROLS: the admitted conversions keep compiling and agree — widening into
// the element type, identical types, and §5.1 constant narrowing of a constant
// operand into an unsigned element.
BEZEL_TEST(range_in_contains_operand_conversion_6853, admitted_conversions_still_compile_spec_5_1) {
    auto r = runAot(prog(
        "    var b: byte := 200;\n"
        "    var s: short := -3;\n"
        "    var i: int := 7;\n"
        "    var u: uint := 7;\n"
        "    var l: long := 7;\n"
        "    Console.WriteLine(b in 100 .. 255);\n"
        "    Console.WriteLine((-5 .. 10).Contains(s));\n"
        "    Console.WriteLine(s in -5 .. 10);\n"
        "    Console.WriteLine(i in -5 .. 10);\n"
        "    Console.WriteLine(u in 1U .. 10U);\n"
        "    Console.WriteLine(u in 1L .. 10L);\n"
        "    Console.WriteLine((1L .. 10L).Contains(i));\n"
        "    Console.WriteLine(l in 1L .. 10L);\n"
        "    Console.WriteLine(5 in 1U .. 10U);\n"
        "    Console.WriteLine((1U .. 10U).Contains(5));\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "True\nTrue\nTrue\nTrue\nTrue\nTrue\nTrue\nTrue\nTrue\nTrue\n");
}
