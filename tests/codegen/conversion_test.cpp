// Codegen slice CG44 — explicit conversions `T(x)` (§5.2). A primitive type
// name applied with call syntax converts its argument: integer width
// changes (trunc / sext / zext), int↔float (si/ui<->fp), and float↔double
// (fpext / fptrunc). Narrowing overflow checks are deferred. Observable via
// the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& body) {
    auto r = runJit(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}
} // namespace

BEZEL_TEST(codegen_cg44, char_to_int) {
    BEZEL_EXPECT_EQ(run("var c := 'A'; return int(c) + 1;"), 66);
}

BEZEL_TEST(codegen_cg44, double_to_int_truncates) {
    BEZEL_EXPECT_EQ(run("var d := 3.9; return int(d);"), 3);
}

BEZEL_TEST(codegen_cg44, int_to_double) {
    BEZEL_EXPECT_EQ(run("var n := 7; var d := double(n); "
                        "return (1 if d = 7.0 else 0);"), 1);
}

BEZEL_TEST(codegen_cg44, long_to_int) {
    BEZEL_EXPECT_EQ(run("var x := 300L; return int(x);"), 300);
}

BEZEL_TEST(codegen_cg44, int_to_byte_truncates) {
    BEZEL_EXPECT_EQ(run("var n := 258; var b := byte(n); return int(b);"), 2);
}

BEZEL_TEST(codegen_cg44, int_to_long_widens) {
    BEZEL_EXPECT_EQ(run("var n := 5; var x := long(n); return int(x);"), 5);
}

BEZEL_TEST(codegen_cg44, negative_int_to_long_sign_extends) {
    BEZEL_EXPECT_EQ(run("var n := -3; var x := long(n); return int(x);"), -3);
}

BEZEL_TEST(codegen_cg44, conversion_in_arithmetic) {
    BEZEL_EXPECT_EQ(run("var d := 2.5; return int(d) * 20 + 2;"), 42);
}
