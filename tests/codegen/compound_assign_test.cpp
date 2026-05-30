// Codegen slice CG31 — compound assignment (`+=`, `-=`, `*=`, `/=`, `&=`,
// `|=`, `^=`, `<<=`, `>>=`) and increment/decrement (`x++`, `x--`). Each
// reads the lvalue, applies the operator (integer `+ - * / mod`
// overflow/zero-checked as usual), and writes back. Targets may be
// locals, static fields, or instance fields. Observable via exit code.

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

BEZEL_TEST(codegen_cg31, plus_equals) {
    BEZEL_EXPECT_EQ(run("var x := 40; x += 2; return x;"), 42);
}

BEZEL_TEST(codegen_cg31, minus_equals) {
    BEZEL_EXPECT_EQ(run("var x := 50; x -= 8; return x;"), 42);
}

BEZEL_TEST(codegen_cg31, times_equals) {
    BEZEL_EXPECT_EQ(run("var x := 6; x *= 7; return x;"), 42);
}

BEZEL_TEST(codegen_cg31, divide_equals) {
    BEZEL_EXPECT_EQ(run("var x := 84; x /= 2; return x;"), 42);
}

BEZEL_TEST(codegen_cg31, bitwise_and_equals) {
    BEZEL_EXPECT_EQ(run("var x := 0xFF; x &= 0x2A; return x;"), 42);
}

BEZEL_TEST(codegen_cg31, shift_left_equals) {
    BEZEL_EXPECT_EQ(run("var x := 21; x <<= 1; return x;"), 42);
}

BEZEL_TEST(codegen_cg31, increment) {
    BEZEL_EXPECT_EQ(run("var x := 41; x++; return x;"), 42);
}

BEZEL_TEST(codegen_cg31, decrement) {
    BEZEL_EXPECT_EQ(run("var x := 43; x--; return x;"), 42);
}

BEZEL_TEST(codegen_cg31, increment_in_loop) {
    BEZEL_EXPECT_EQ(run(
        "var x := 0; var i := 0; "
        "while i < 5 do x += 10; i++; end; return x;"), 50);
}

BEZEL_TEST(codegen_cg31, compound_on_static_field) {
    auto r = runJit(
        "module M; class C "
        "  public static _n: int := 100; "
        "  public static method Main(): int "
        "    _n += 5; _n *= 2; return _n; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 210);
}

BEZEL_TEST(codegen_cg31, compound_on_instance_field) {
    auto r = runJit(
        "module M; class C "
        "  public _v: int := 10; "
        "  public method Bump() _v += 32; end Bump; "
        "  public method Get(): int => _v; "
        "  public static method Main(): int "
        "    var c := new C(); c.Bump(); return c.Get(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 42);
}
