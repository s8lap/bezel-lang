// Codegen slice CG31 — compound assignment (`+=`, `-=`, `*=`, `/=`, `&=`,
// `|=`, `^=`, `<<=`, `>>=`) and increment/decrement (`x++`, `x--`). Each
// reads the lvalue, applies the operator (integer `+ - * / mod`
// overflow/zero-checked as usual), and writes back. Targets may be
// locals, static fields, or instance fields. Observable via exit code.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& body) {
    auto r = runAot(
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
    auto r = runAot(
        "module M; class C "
        "  public static _n: int := 100; "
        "  public static method Main(): int "
        "    _n += 5; _n *= 2; return _n; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 210);
}

BEZEL_TEST(codegen_cg31, compound_on_instance_field) {
    auto r = runAot(
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

// #1041 / §6.5 / §6.6 — the compound shift assignments. ⚠ THE RULE CHANGED AT
// v0.242 (proposal 0079, #3045). #1041 fixed `<<=` / `>>=` to reduce the count
// modulo the operand's bit width, matching the non-compound forms (before it,
// emitCompoundOp emitted a raw `shl`/`lshr`/`ashr` with an unmasked count, so
// `x <<= 40` on an i32 produced poison). §6.6 now says instead: "`x <<= k` and
// `x >>= k` inherit §6.5's count-domain rule through the desugaring: an
// out-of-domain count is the same fault — compile error for a constant count,
// `OverflowException` at runtime otherwise." So each cell below flips to the
// arm its count's form selects, and the poison #1041 removed stays removed —
// the guarded path never reaches `shl` with an out-of-domain count.

namespace {
// The compound form's §6.5 constant-count reject.
bool rejectsCompoundShiftDomain(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("is outside the domain") != std::string::npos &&
            d.message.find("(\xC2\xA7""6.5/\xC2\xA7""17.14)") !=
                std::string::npos)
            return true;
    return false;
}
// 1 = the body raised OverflowException, 0 = ran to completion.
int compoundShiftThrows(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int "
                    "  try ") +
        body +
        " return 0; catch OverflowException => return 1; end; return 0; "
        "end Main; end C;");
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §6.5/§6.6/§17.14 (v0.242): a CONSTANT count of 40 on an `int` target is
// outside `0..31` → compile error. WAS: `x <<= 40` gave 256 (40 mod 32 == 8).
BEZEL_TEST(codegen_1041_compound_shift, shl_const_count_40_rejected_3045) {
    BEZEL_EXPECT_TRUE(rejectsCompoundShiftDomain("var x: int := 1; x <<= 40;"));
}
// §6.5/§6.6 (v0.242): same for `>>=`. WAS: `x >>= 40` gave 1 (256 >> 8).
BEZEL_TEST(codegen_1041_compound_shift, shr_const_count_40_rejected_3045) {
    BEZEL_EXPECT_TRUE(
        rejectsCompoundShiftDomain("var x: int := 256; x >>= 40;"));
}
// §6.5/§6.6/§31.4 (v0.242): the RUNTIME arm — a variable count of 40 raises
// OverflowException. WAS: `x <<= y` with y = 40 gave 256.
BEZEL_TEST(codegen_1041_compound_shift, shl_runtime_count_40_raises_3045) {
    BEZEL_EXPECT_EQ(compoundShiftThrows(
        "var x: int := 1; var y: int := 40; x <<= y;"), 1);
}
// §6.5/§6.6 (v0.242): `long`'s domain is `0..63`, so a constant 65 is outside
// it. WAS: `x <<= 65` on a long gave 2 (65 mod 64 == 1).
BEZEL_TEST(codegen_1041_compound_shift, long_const_count_65_rejected_3045) {
    BEZEL_EXPECT_TRUE(
        rejectsCompoundShiftDomain("var x: long := 1; x <<= 65;"));
}
// §6.6 (v0.242): `x <<= k` faults IDENTICALLY to its desugaring `x := x << k`
// — the same count, the same runtime fault, on both spellings.
BEZEL_TEST(codegen_1041_compound_shift, compound_matches_desugaring_3045) {
    BEZEL_EXPECT_EQ(compoundShiftThrows(
        "var x: int := 1; var y: int := 40; x <<= y;"), 1);
    BEZEL_EXPECT_EQ(compoundShiftThrows(
        "var x: int := 1; var y: int := 40; x := x << y;"), 1);
}
BEZEL_TEST(codegen_1041_compound_shift, ordinary_shift_still_correct) {
    BEZEL_EXPECT_EQ(run("var x: int := 1; x <<= 4; return x;"), 16);
}
// §6.5 (v0.242): the domain's upper edge on a `long` target — 63 shifts.
BEZEL_TEST(codegen_1041_compound_shift, long_count_63_in_domain_3045) {
    BEZEL_EXPECT_EQ(run("var x: long := 1; x <<= 63; "
                        "return (if x < 0L then 1 else 0);"), 1);
}

// #1042: compound arithmetic/bitwise/shift assignment on a sub-i32 integer type
// (byte/sbyte/short/ushort) crashed codegen — the RHS is lowered at its natural
// i32 width, so `byte b += 3` called `llvm.uadd.with.overflow.i8` with an i32
// operand (malformed IR). emitCompoundOp now coerces the RHS to the LHS integer
// type, so the op runs at the LHS width with the correct overflow check (matching
// how `b++`/`b--` already worked). In-range RHS values are correct; an LHS-width
// overflow (200 += 100 = 300 > 255) raises OverflowException as for any byte.
BEZEL_TEST(codegen_1042_subint_compound, byte_plus_equals) {
    BEZEL_EXPECT_EQ(run("var b: byte := 5; b += 3; return int(b);"), 8);
}
BEZEL_TEST(codegen_1042_subint_compound, byte_minus_equals) {
    BEZEL_EXPECT_EQ(run("var b: byte := 5; b -= 2; return int(b);"), 3);
}
BEZEL_TEST(codegen_1042_subint_compound, byte_times_equals) {
    BEZEL_EXPECT_EQ(run("var b: byte := 5; b *= 3; return int(b);"), 15);
}
BEZEL_TEST(codegen_1042_subint_compound, short_plus_equals) {
    BEZEL_EXPECT_EQ(run("var s: short := 10; s += 5; return int(s);"), 15);
}
BEZEL_TEST(codegen_1042_subint_compound, ushort_plus_equals) {
    BEZEL_EXPECT_EQ(run("var u: ushort := 100; u += 50; return int(u);"), 150);
}
BEZEL_TEST(codegen_1042_subint_compound, byte_bitand_equals) {
    BEZEL_EXPECT_EQ(run("var b: byte := 5; b &= 3; return int(b);"), 1);
}
BEZEL_TEST(codegen_1042_subint_compound, byte_shl_equals) {
    BEZEL_EXPECT_EQ(run("var b: byte := 5; b <<= 1; return int(b);"), 10);
}
// LHS-width overflow (200 + 100 = 300 > byte max) raises OverflowException.
BEZEL_TEST(codegen_1042_subint_compound, byte_plus_overflow_throws) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var b: byte := 200; "
        "  try b += 100; return 0; catch OverflowException => return 1; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}

// #1043 / §6.1: an arithmetic compound assignment on a sub-i32 integer is
// `b = subInt(b OP v)` with CHECKED narrowing — an out-of-range result raises
// OverflowException. #1042 truncated the RHS to the LHS width first, so an
// out-of-range RHS gave a wrong value (`byte 5 += 300` returned 49). It now
// computes at i32 and narrow-checks the result, throwing on overflow.
namespace {
// Returns 1 if the body raises OverflowException, 0 otherwise (or -999 on
// front-end failure).
int throwsOverflow(const std::string& stmt) {
    auto r = bezel::test::runAot(
        std::string("module M; class C public static method Main(): int "
                    "  try ") + stmt +
        " return 0; catch OverflowException => return 1; end; return 0; "
        "end Main; end C;");
    return r.ran ? r.exitCode : -999;
}
}  // namespace
BEZEL_TEST(codegen_1043_subint_overflow, byte_plus_out_of_range_literal_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow("var b: byte := 5; b += 300;"), 1);
}
BEZEL_TEST(codegen_1043_subint_overflow, byte_plus_sum_overflow_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow("var b: byte := 200; b += 100;"), 1);
}
BEZEL_TEST(codegen_1043_subint_overflow, byte_times_overflow_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow("var b: byte := 5; b *= 100;"), 1);
}
// In-range arithmetic does NOT throw and is correct.
BEZEL_TEST(codegen_1043_subint_overflow, byte_in_range_no_throw) {
    BEZEL_EXPECT_EQ(run("var b: byte := 5; b += 250; return int(b);"), 255);
}
BEZEL_TEST(codegen_1043_subint_overflow, byte_div_large_divisor_no_throw) {
    // 10 / 100 = 0 — a large divisor must NOT spuriously raise.
    BEZEL_EXPECT_EQ(run("var b: byte := 10; b /= 100; return int(b);"), 0);
}
