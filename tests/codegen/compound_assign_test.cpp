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

// #1041 / §6.5: the compound shift assignments `<<=` / `>>=` must reduce the
// shift count modulo the operand's bit width, exactly as the non-compound
// `<<` / `>>` do (L538). Before the fix, emitCompoundOp emitted a raw
// `shl`/`lshr`/`ashr` with the unmasked count, so `x <<= 40` on an i32
// produced poison (a wrong value) while `x << 40` correctly gave `x << 8`.
BEZEL_TEST(codegen_1041_compound_shift, shl_count_reduced_mod_width) {
    // 40 mod 32 = 8 → 1 << 8 = 256.
    BEZEL_EXPECT_EQ(run("var x: int := 1; x <<= 40; return x;"), 256);
}
BEZEL_TEST(codegen_1041_compound_shift, shr_count_reduced_mod_width) {
    // 40 mod 32 = 8 → 256 >> 8 = 1.
    BEZEL_EXPECT_EQ(run("var x: int := 256; x >>= 40; return x;"), 1);
}
BEZEL_TEST(codegen_1041_compound_shift, shl_variable_count_reduced) {
    BEZEL_EXPECT_EQ(run("var x: int := 1; var y: int := 40; x <<= y; return x;"), 256);
}
BEZEL_TEST(codegen_1041_compound_shift, long_count_reduced_mod_64) {
    // long is 64-bit: 65 mod 64 = 1 → 1 << 1 = 2 (confirms per-type width).
    BEZEL_EXPECT_EQ(run("var x: long := 1; x <<= 65; return int(x);"), 2);
}
BEZEL_TEST(codegen_1041_compound_shift, ordinary_shift_still_correct) {
    BEZEL_EXPECT_EQ(run("var x: int := 1; x <<= 4; return x;"), 16);
}
