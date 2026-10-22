// fix #6959 -- sema: `Math.Min/Max/Clamp<T>` with an explicit type list.
// Spec §14.5 (v0.340): with an explicit list each argument converts to T at
// its conversion site; §5.1: only an in-range COMPILE-TIME CONSTANT narrows, a
// run-time `int` needs `byte(x)`, and an int constant never converts to `char`
// (`var x: char := 98;` is rejected). Before the fix `Math.Max<byte>(b, i)`
// (non-constant `int i`) and `Math.Max<char>(c, 98)` were accepted.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
bool rejectedWith(const bezel::test::JitResult& r, const std::string& needle) {
    if (!failedAtCompileTimeWithRealLocation(r)) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

std::string prog(const std::string& decls, const std::string& body) {
    return "module M; class C public static method Main(): int " + decls +
           " " + body + " return 0; end Main; end C;";
}
std::string out(const std::string& decls, const std::string& body) {
    auto r = runAot(prog(decls, body));
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

BEZEL_TEST(sema_math_minmax_explicit_list_6959, byte_with_nonconst_int_rejected_5_1) {
    auto r = runAot(prog("var b: byte := 5; var i: int := 7;",
                         "var r: byte := Math.Max<byte>(b, i); "
                         "Console.WriteLine(r.ToString());"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "Math.Max<byte>"));
}
BEZEL_TEST(sema_math_minmax_explicit_list_6959, clamp_byte_with_nonconst_bound_rejected_5_1) {
    auto r = runAot(prog("var b: byte := 5; var i: int := 7;",
                         "var r: byte := Math.Clamp<byte>(b, 1, i); "
                         "Console.WriteLine(r.ToString());"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "Math.Clamp<byte>"));
}
BEZEL_TEST(sema_math_minmax_explicit_list_6959, uint_with_int_variable_rejected_5_1) {
    auto r = runAot(prog("var u: uint := 5u; var i: int := 7;",
                         "Console.WriteLine(Math.Min<uint>(u, i));"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "Math.Min<uint>"));
}
BEZEL_TEST(sema_math_minmax_explicit_list_6959, char_with_int_literal_rejected_5_1) {
    auto r = runAot(prog("var c: char := 'a';",
                         "Console.WriteLine(Math.Max<char>(c, 98));"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "Math.Max<char>"));
}
BEZEL_TEST(sema_math_minmax_explicit_list_6959, var_char_from_int_literal_still_rejected_control) {
    auto r = runAot(prog("", "var x: char := 98; Console.WriteLine(x);"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// --- accept controls: conversions §5.1 allows ---

BEZEL_TEST(sema_math_minmax_explicit_list_6959, widening_operands_accepted_5_1) {
    BEZEL_EXPECT_EQ(out("var l: long := 5000000000L; var i: int := 7; "
                        "var b: byte := 9; var s: short := 3;",
                        "Console.WriteLine(Math.Max<long>(l, i)); "
                        "Console.WriteLine(Math.Min<int>(i, b)); "
                        "Console.WriteLine(Math.Max<int>(s, b));"),
                    std::string("5000000000\n7\n9\n"));
}
BEZEL_TEST(sema_math_minmax_explicit_list_6959, float_with_int_widening_accepted_5_1) {
    BEZEL_EXPECT_EQ(out("var f: float := 1.5f;",
                        "Console.WriteLine(Math.Max<float>(f, 2)); "
                        "Console.WriteLine(Math.Min<float>(f, 2));"),
                    std::string("2\n1.5\n"));
}
BEZEL_TEST(sema_math_minmax_explicit_list_6959, in_range_constant_for_byte_accepted_5_1) {
    BEZEL_EXPECT_EQ(out("const N: int := 200; var b: byte := 3;",
                        "Console.WriteLine(Math.Max<byte>(b, N)); "
                        "Console.WriteLine(Math.Max<byte>(b, 7));"),
                    std::string("200\n7\n"));
}
BEZEL_TEST(sema_math_minmax_explicit_list_6959, char_operands_accepted) {
    BEZEL_EXPECT_EQ(out("var c: char := 'a'; var d: char := 'b';",
                        "Console.WriteLine(Math.Max<char>(c, d));"),
                    std::string("b\n"));
}
