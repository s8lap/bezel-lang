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

// `long` and `ulong` are wider than the `int` element: never implicit.
BEZEL_TEST(range_in_contains_operand_conversion_6853, long_and_ulong_into_int_range_spec_5_1) {
    auto r1 = runAot(prog(
        "    var l: long := 3;\n"
        "    Console.WriteLine(l in 0 .. 10);\n"));
    BEZEL_EXPECT_FALSE(r1.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r1, kInSite));
    auto r2 = runAot(prog(
        "    var g: ulong := 3;\n"
        "    Console.WriteLine((0 .. 10).Contains(g));\n"));
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

// A signed operand against an unsigned element type.
BEZEL_TEST(range_in_contains_operand_conversion_6853, int_into_uint_range_spec_5_1) {
    auto r = runAot(prog(
        "    var i: int := 3;\n"
        "    Console.WriteLine(i in 1U .. 10U);\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kInSite));
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
