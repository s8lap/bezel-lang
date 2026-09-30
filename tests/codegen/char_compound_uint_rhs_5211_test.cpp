// Codegen — #5211 (§6.6, v0.313): runtime controls around the sema rule that a
// non-constant uint/ulong right operand of a `char` compound target is a
// compile error (pinned in tests/lit/sema/char_compound_uint_ulong_rhs_*_5211).
// The repro `c += u` (u = 4294967295) used to print 99 instead of raising.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string run(const std::string& body) {
    auto r = runAot("module M; class C public static method Main(): int " + body +
                    " return 0; end Main; end C;");
    if (!r.ran) return "NORUN";
    std::string s = r.stdoutText;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}
}  // namespace

// The issue's repro must no longer compile and run to a silent wrong char.
BEZEL_TEST(codegen_5211_char_compound_uint, repro_rejected_not_silent_99) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "var c: char := char(100); var u: uint := 4294967295; "
        "try c += u; Console.WriteLine(int(c)); "
        "catch OverflowException => Console.WriteLine(\"OVF-compound\"); end; "
        "return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("99") == std::string::npos);
}
BEZEL_TEST(codegen_5211_char_compound_uint, int_rhs_valid) {
    BEZEL_EXPECT_EQ(run("var c: char := char(100); var i: int := 5; c += i; "
                        "Console.WriteLine(int(c));"), std::string("105"));
}
BEZEL_TEST(codegen_5211_char_compound_uint, long_rhs_out_of_range_raises) {
    BEZEL_EXPECT_EQ(run("var c: char := char(100); var l: long := 4294967295; "
                        "try c += l; Console.WriteLine(int(c)); "
                        "catch OverflowException => Console.WriteLine(\"OVF\"); end;"),
                    std::string("OVF"));
}
BEZEL_TEST(codegen_5211_char_compound_uint, small_constant_uint_valid) {
    BEZEL_EXPECT_EQ(run("var c: char := char(100); c += 5; c -= 2; c++; "
                        "Console.WriteLine(int(c));"), std::string("104"));
}
