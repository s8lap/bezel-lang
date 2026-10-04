// fix #4855 / §6.4 / §6.1: bz_decimal_mod must not abort when scale alignment
// rounds the divisor's mantissa to zero, and must return the CORRECT remainder.
//
// The previous lane's fix computed rem = ma % mb at scale sa, which is
// mathematically wrong: the true remainder of a mod b (where a=ma*10^(-sa),
// b=mb*10^(-sb), sb>sa) is r' = (ma*10^(sb-sa)) % mb at scale sb, so
// r = r' * 10^(-sb). For MaxValue mod 0.4: ((2^96-1)*10 % 4)*0.1 = 0.2.
// The wrong formula gave 3.0 (using ma%mb=3 at scale 0).
//
// Spec references: §6.1 (mod follows dividend-sign rule),
// §6.4 (decimal arithmetic, no abort, OverflowException on overflow).

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}
} // namespace

// fix #4855: decimal.MaxValue mod 0.4m must not abort AND must return
// the correct remainder. Python (60-digit Decimal) confirms: 0.2.
// The wrong fix returned 3.0 (stored ma%mb=3 at scale 0, forgetting
// that 0.4 = 4*10^(-1) not 4*10^0, and that the remainder must be at
// the divisor's scale).
BEZEL_TEST(codegen_4855_rt, decimal_maxvalue_mod_04m_correct_value) {
    auto r = runAot(prog(
        "Console.WriteLine(decimal.MaxValue mod 0.4m); "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0.2\n");
}

// fix #4855: also verify the non-constant-variable form produces the same.
BEZEL_TEST(codegen_4855_rt, decimal_maxvalue_mod_literal_04m_correct_value) {
    auto r = runAot(prog(
        "var a := decimal.MaxValue; "
        "Console.WriteLine(a mod 0.4m); "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0.2\n");
}

// Sanity: simple decimal mod still works correctly.
// 10.0m mod 3.0m = 1.0m per §6.1 (10 = 3*3 + 1).
BEZEL_TEST(codegen_4855_rt, decimal_mod_still_works) {
    auto r = runAot(prog(
        "var a := 10.0m; "
        "var b := 3.0m; "
        "var c := a mod b; "
        "if c = 1.0m then return 1; end; "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
