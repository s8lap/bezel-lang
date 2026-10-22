// fix #6909 -- sema: `Math.Min/Max/Clamp<T>(a: T, b: T)` (stdlib §2.2, v0.340)
// with operands of different types. Spec §14.5 "Agreement" (v0.340): "All
// bindings a type parameter receives ... must be the same type", and
// "`Pick(b, 250)` for `b: byte` is an error — `250` is an `int`; §5.1's
// constant narrowing is a conversion-site rule and never participates in a
// join. With an explicit list — `Pick<byte>(b, 250)` — the parameter type is
// `byte` and the literal constant-narrows at the argument conversion site".
// Before the fix the mixed calls reached codegen: `Math.Max(b, 7)` and
// `Math.Max(l, 3)` failed the IR verifier, `Math.Max(u, i)` compared signed
// and `Math.Min(u, -1)` printed 5.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
const char* kAgree = "inconsistent type-argument inference: type parameter `T` of `Math.";

bool rejectedWith(const bezel::test::JitResult& r, const std::string& needle) {
    if (!failedAtCompileTimeWithRealLocation(r)) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

std::string prog(const std::string& decls, const std::string& ty,
                 const std::string& call) {
    return "module M; class C public static method Main(): int " + decls +
           " var r: " + ty + " := " + call +
           "; Console.WriteLine(r.ToString()); return 0; end Main; end C;";
}
}  // namespace

// --- reject rows: inferred T, operands disagree (§14.5 Agreement) ---

BEZEL_TEST(sema_math_minmax_mixed_operands_6909, byte_with_int_literal_rejected_14_5) {
    auto r = runAot(prog("var b: byte := 5;", "byte", "Math.Max(b, 7)"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, kAgree));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "both `byte` and `int`"));
}
BEZEL_TEST(sema_math_minmax_mixed_operands_6909, byte_with_out_of_range_literal_rejected_14_5) {
    // was an IR verifier failure (icmp i8 vs i32)
    BEZEL_EXPECT_TRUE(rejectedWith(
        runAot(prog("var b: byte := 200;", "byte", "Math.Max(b, 300)")), kAgree));
    BEZEL_EXPECT_TRUE(rejectedWith(
        runAot(prog("var b: byte := 200;", "byte", "Math.Max(b, -1)")), kAgree));
}
BEZEL_TEST(sema_math_minmax_mixed_operands_6909, uint_with_int_literal_rejected_14_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        runAot(prog("var u: uint := 5u;", "uint", "Math.Max(u, 7)")), kAgree));
    // printed 5 before the fix
    BEZEL_EXPECT_TRUE(rejectedWith(
        runAot(prog("var u: uint := 5u;", "uint", "Math.Min(u, -1)")), kAgree));
}
BEZEL_TEST(sema_math_minmax_mixed_operands_6909, long_with_int_literal_rejected_14_5) {
    auto r = runAot(prog("var l: long := 5L;", "long", "Math.Max(l, 3)"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "both `long` and `int`"));
}
BEZEL_TEST(sema_math_minmax_mixed_operands_6909, uint_with_int_variable_rejected_14_5) {
    auto r = runAot(prog("var u: uint := 4000000000u; var i: int := 3;", "uint",
                         "Math.Max(u, i)"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "both `uint` and `int`"));
}
BEZEL_TEST(sema_math_minmax_mixed_operands_6909, clamp_mixed_operands_rejected_14_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        runAot(prog("var b: byte := 5;", "byte", "Math.Clamp(b, 1, 9)")),
        "type parameter `T` of `Math.Clamp`"));
}
BEZEL_TEST(sema_math_minmax_mixed_operands_6909, const_int_beside_byte_rejected_14_5) {
    // a constant contributes its static type, `int`
    BEZEL_EXPECT_TRUE(rejectedWith(
        runAot(prog("const N: int := 7; var b: byte := 5;", "byte", "Math.Max(b, N)")),
        kAgree));
}
BEZEL_TEST(sema_math_minmax_mixed_operands_6909, double_with_int_literal_rejected_14_5) {
    // §14.5: no widened join — `Pick(1, 2L)` is an error, not `long`
    auto r = runAot(prog("var d: double := 5.5;", "double", "Math.Max(d, 3)"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "both `double` and `int`"));
}

// --- explicit list: the literal constant-narrows at the conversion site ---

BEZEL_TEST(sema_math_minmax_mixed_operands_6909, explicit_out_of_range_literal_rejected_5_1) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        runAot(prog("var b: byte := 5;", "byte", "Math.Max<byte>(b, 300)")),
        "constant 300 is out of range for the type argument `byte`"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        runAot(prog("var u: uint := 5u;", "uint", "Math.Min<uint>(u, -1)")),
        "constant -1 is out of range for the type argument `uint`"));
}
BEZEL_TEST(sema_math_minmax_mixed_operands_6909, explicit_byte_list_narrows_literal_14_5) {
    auto r = runAot(prog("var b: byte := 200;", "byte", "Math.Max<byte>(b, 7)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
    auto c = runAot(prog("var b: byte := 200;", "byte", "Math.Clamp<byte>(b, 1, 9)"));
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.stdoutText, std::string("9\n"));
}
BEZEL_TEST(sema_math_minmax_mixed_operands_6909, explicit_long_and_sbyte_lists_14_5) {
    auto l = runAot(prog("var l: long := 5L;", "long", "Math.Min<long>(l, -3)"));
    BEZEL_EXPECT_TRUE(l.ran);
    BEZEL_EXPECT_EQ(l.stdoutText, std::string("-3\n"));
    auto s = runAot(prog("var s: sbyte := 5;", "sbyte", "Math.Min<sbyte>(s, -128)"));
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.stdoutText, std::string("-128\n"));
}

// --- accept controls: operands agree ---

BEZEL_TEST(sema_math_minmax_mixed_operands_6909, agreeing_operands_accepted_14_5) {
    auto i = runAot(prog("var i: int := 5;", "int", "Math.Max(i, 7)"));
    BEZEL_EXPECT_TRUE(i.ran);
    BEZEL_EXPECT_EQ(i.stdoutText, std::string("7\n"));
    auto b = runAot(prog("var b: byte := 200; var c: byte := 9;", "byte", "Math.Min(b, c)"));
    BEZEL_EXPECT_TRUE(b.ran);
    BEZEL_EXPECT_EQ(b.stdoutText, std::string("9\n"));
    auto d = runAot(prog("var d: double := 5.5;", "double", "Math.Max(d, 3.0)"));
    BEZEL_EXPECT_TRUE(d.ran);
    BEZEL_EXPECT_EQ(d.stdoutText, std::string("5.5\n"));
}
