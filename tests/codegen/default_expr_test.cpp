// Codegen slice CG37 — `default(T)` (§A.7). The default expression is the
// zero value of T: 0 for integers, false for bool, 0.0 for float/double,
// and a null reference for class types. Observable via the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& body) {
    auto r = runJit(
        std::string("module M; class T end T; "
                    "class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg37, default_int_is_zero) {
    BEZEL_EXPECT_EQ(run("var x := default(int); return x + 42;"), 42);
}

BEZEL_TEST(codegen_cg37, default_bool_is_false) {
    BEZEL_EXPECT_EQ(run(
        "var b := default(bool); if b then return 1; end; return 7;"), 7);
}

BEZEL_TEST(codegen_cg37, default_double_is_zero) {
    BEZEL_EXPECT_EQ(run(
        "var d := default(double); return (5 if d = 0.0 else 0);"), 5);
}

BEZEL_TEST(codegen_cg37, default_reference_is_null) {
    // §17.11 (#951): `default(T)` for a non-nullable reference type is a compile
    // error; the null value of a reference type is `default(T?)` (the nullable
    // counterpart). T is `class T`; `default(T?)` is null.
    BEZEL_EXPECT_EQ(run(
        "var t: T? := default(T?); "
        "try var u := t!; return 0; "
        "catch NullReferenceException => return 9; end;"), 9);
}

BEZEL_TEST(codegen_cg37, default_in_arithmetic) {
    BEZEL_EXPECT_EQ(run(
        "return default(int) + 6 * 7;"), 42);
}
