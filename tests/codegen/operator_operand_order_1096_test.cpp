// Regression tests for #1096 (§25): a binary operator overload whose operands
// did not match its declared parameter ORDER (e.g. `4 * v` against
// `operator *(Vec, int)`) was accepted by sema (matched on the right operand's
// type) but emitted args in source order at codegen → an IR-verification crash.
// Bezel does not auto-commute operator overloads (§25 defines none, and a swap
// would be unsound for non-commutative operators like `-`), so the operands
// must match the overload's parameter order; a mismatch is now a clean §25
// compile error instead of a crash. Found by adversarial discovery sweep #7.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
const char* VEC =
    "module M; "
    "record Vec(X: int, Y: int) "
    "  public static method operator *(v: Vec, scalar: int): Vec => "
    "    new Vec(v.X * scalar, v.Y * scalar); "
    "  public static method operator +(a: Vec, b: Vec): Vec => "
    "    new Vec(a.X + b.X, a.Y + b.Y); "
    "end Vec; ";
}  // namespace

// #1096: a reversed-operand use (`4 * v` with only `operator *(Vec, int)`) is a
// clean §25 compile error — NOT an IR-verification crash.
BEZEL_TEST(operator_operand_order_1096, reversed_operands_rejected_not_crash) {
    auto r = runJit(
        std::string(VEC) +
        "class Mn public static method Main(): int "
        "  var v := new Vec(2, 3); "
        "  var r := 4 * v; "                              // (int, Vec) ≠ (Vec, int)
        "  return r.X; "
        "end Main; end Mn;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The correctly-ordered use compiles and computes.
BEZEL_TEST(operator_operand_order_1096, correct_order_works) {
    BEZEL_EXPECT_EQ(run(
        std::string(VEC) +
        "class Mn public static method Main(): int "
        "  var v := new Vec(2, 3); "
        "  var r := v * 4; "                              // (Vec, int) → (8, 12)
        "  return r.X; "
        "end Main; end Mn;"), 8);
}

// A same-type operator (record + record) is unaffected.
BEZEL_TEST(operator_operand_order_1096, record_plus_record_works) {
    BEZEL_EXPECT_EQ(run(
        std::string(VEC) +
        "class Mn public static method Main(): int "
        "  var r := new Vec(2, 3) + new Vec(10, 20); "    // (12, 23)
        "  return r.X; "
        "end Main; end Mn;"), 12);
}

// An undeclared operator on a record operand is a clean error (no crash).
BEZEL_TEST(operator_operand_order_1096, undeclared_operator_rejected) {
    auto r = runJit(
        std::string(VEC) +
        "class Mn public static method Main(): int "
        "  var v := new Vec(2, 3); "
        "  var r := v - new Vec(1, 1); "                  // no operator -
        "  return r.X; "
        "end Main; end Mn;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Regression guard: built-in arithmetic on primitives (and string `+`) is
// unaffected by the record/class operator reject.
BEZEL_TEST(operator_operand_order_1096, builtin_arithmetic_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"ab\" + \"cd\"; "
        "  var d: decimal := 1.5m + 2.5m; "
        "  return s.Length + int(d); "                    // 4 + 4 = 8
        "end Main; end C;"), 8);
}
