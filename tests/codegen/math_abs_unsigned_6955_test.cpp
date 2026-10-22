// Regression tests for #6955 -- `Math.Abs` of an unsigned (or narrow) operand
// was lowered at the operand's own width: byte 200 printed 56, uint
// 4000000000 printed 294967296.
//
// stdlib §2.2 (v0.340) declares exactly four overloads, `Abs(int)`,
// `Abs(long)`, `Abs(double)`, `Abs(decimal)`. Spec §22.6: every implicit
// conversion ranks co-equal, so an operand that is none of those four types
// and converts to several of them is an AMBIGUOUS call (stdlib rationale §2.2:
// "a `short` or `sbyte` argument, co-equal across all four ... a §22.6
// ambiguity"). §5.1 conversion table: byte/ushort/char -> int, long, double,
// decimal; uint -> long, double, decimal; ulong -> double, decimal only.
// float has one applicable overload (double); the four declared types are exact.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
bool ambiguous(const bezel::test::JitResult& r) {
    if (!failedAtCompileTimeWithRealLocation(r)) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("ambiguous call to `Math.Abs`") != std::string::npos &&
            d.message.find("§22.6") != std::string::npos)
            return true;
    return false;
}
std::string prog(const std::string& body) {
    return "module M; import System; class C public static method Main(): int " +
           body + " return 0; end Main; end C;";
}
bool rejects(const std::string& body) { return ambiguous(runAot(prog(body))); }
std::string out(const std::string& body) {
    auto r = runAot(prog(body));
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

BEZEL_TEST(math_abs_unsigned_6955, byte_issue_repro_rejected) {
    BEZEL_EXPECT_TRUE(rejects("Console.WriteLine(Math.Abs(byte(200)));"));
}
BEZEL_TEST(math_abs_unsigned_6955, byte_variable_rejected) {
    BEZEL_EXPECT_TRUE(rejects("var b: byte := 200; Console.WriteLine(Math.Abs(b));"));
}
BEZEL_TEST(math_abs_unsigned_6955, ushort_rejected) {
    BEZEL_EXPECT_TRUE(rejects("Console.WriteLine(Math.Abs(ushort(60000)));"));
}
BEZEL_TEST(math_abs_unsigned_6955, uint_rejected) {
    BEZEL_EXPECT_TRUE(rejects("Console.WriteLine(Math.Abs(4000000000u));"));
}
BEZEL_TEST(math_abs_unsigned_6955, ulong_rejected) {
    BEZEL_EXPECT_TRUE(rejects("Console.WriteLine(Math.Abs(18000000000000000000ul));"));
}
BEZEL_TEST(math_abs_unsigned_6955, sbyte_rejected) {
    BEZEL_EXPECT_TRUE(rejects("var s: sbyte := sbyte(-128); Console.WriteLine(Math.Abs(s));"));
}
BEZEL_TEST(math_abs_unsigned_6955, short_rejected) {
    BEZEL_EXPECT_TRUE(rejects("var s: short := short(-5); var y := Math.Abs(s); Console.WriteLine(y);"));
}
BEZEL_TEST(math_abs_unsigned_6955, char_rejected) {
    BEZEL_EXPECT_TRUE(rejects("Console.WriteLine(Math.Abs('a'));"));
}

// Controls: the four declared overloads, plus float (one applicable: double),
// and the explicit-cast remedy the diagnostic names.
BEZEL_TEST(math_abs_unsigned_6955, declared_overloads_control) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine(Math.Abs(-200)); "
                        "Console.WriteLine(Math.Abs(-5000000000L)); "
                        "Console.WriteLine(Math.Abs(-2.5)); "
                        "Console.WriteLine(Math.Abs(-2.5m)); "
                        "Console.WriteLine(Math.Abs(-1.5f));"),
                    std::string("200\n5000000000\n2.5\n2.5\n1.5\n"));
}
BEZEL_TEST(math_abs_unsigned_6955, explicit_cast_remedy_control) {
    BEZEL_EXPECT_EQ(out("var b: byte := 200; var u: uint := 4000000000u; "
                        "Console.WriteLine(Math.Abs(int(b))); "
                        "Console.WriteLine(Math.Abs(long(u)));"),
                    std::string("200\n4000000000\n"));
}
