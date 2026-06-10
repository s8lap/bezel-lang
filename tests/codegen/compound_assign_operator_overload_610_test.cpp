// Codegen fix #610 — compound assignment (`+=`, `-=`, `*=`, …) on a record
// with a user-defined binary operator overload must desugar to a call of the
// user overload, not emit llvm.sadd.with.overflow on the record struct.
//
// Per §25.1 (v0.134): "the compound forms desugar mechanically to
// read-modify-write using the underlying binary operator's overload."
//
// Spec authority: §6.6 (compound assignments), §25 (operator overloading),
// §25.1 (non-overloadable list / desugar rule).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

// §25.1 / §6.6: `wallet += x` ≡ `wallet := wallet + x` via user overload.
// Exact repro from issue #610: positional record with operator +.
BEZEL_TEST(codegen_fix610, compound_assign_operator_overload_610_plus_record) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Money(Amount: int) "
        "  public static method operator +(a: Money, b: Money): Money => "
        "    new Money(a.Amount + b.Amount); "
        "end Money; "
        "class Program "
        "  public static method Main(): int "
        "    var wallet := new Money(100); "
        "    wallet += new Money(50); "
        "    return wallet.Amount; "
        "  end Main; "
        "end Program;"),
        150);
}

// §25.1 / §6.6: `-=` on a record with operator -.
BEZEL_TEST(codegen_fix610, compound_assign_operator_overload_610_minus_record) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Money(Amount: int) "
        "  public static method operator -(a: Money, b: Money): Money => "
        "    new Money(a.Amount - b.Amount); "
        "end Money; "
        "class Program "
        "  public static method Main(): int "
        "    var wallet := new Money(100); "
        "    wallet -= new Money(42); "
        "    return wallet.Amount; "
        "  end Main; "
        "end Program;"),
        58);
}

// §25.1 / §6.6: `*=` on a record with operator *.
BEZEL_TEST(codegen_fix610, compound_assign_operator_overload_610_times_record) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Scale(Factor: int) "
        "  public static method operator *(a: Scale, b: Scale): Scale => "
        "    new Scale(a.Factor * b.Factor); "
        "end Scale; "
        "class Program "
        "  public static method Main(): int "
        "    var s := new Scale(3); "
        "    s *= new Scale(7); "
        "    return s.Factor; "
        "  end Main; "
        "end Program;"),
        21);
}

// §25.1 / §6.6: compound assignment inside a loop — operator overload called
// each iteration.
BEZEL_TEST(codegen_fix610, compound_assign_operator_overload_610_loop) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Money(Amount: int) "
        "  public static method operator +(a: Money, b: Money): Money => "
        "    new Money(a.Amount + b.Amount); "
        "end Money; "
        "class Program "
        "  public static method Main(): int "
        "    var total := new Money(0); "
        "    var i := 0; "
        "    while i < 5 do "
        "      total += new Money(10); "
        "      i++; "
        "    end; "
        "    return total.Amount; "
        "  end Main; "
        "end Program;"),
        50);
}

// Regression: plain int `+=` must still use overflow-checked intrinsic (§6.1).
// The user-overload path must not intercept primitives.
BEZEL_TEST(codegen_fix610, compound_assign_operator_overload_610_int_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 40; x += 2; return x; "
        "end Main; end C;"),
        42);
}

// §25.1 / §6.6: compound `+=` on a CLASS type with operator +.
// (Class records are heap-allocated, so the current value is a pointer; the
// overload call must receive the loaded struct value, not the pointer.)
BEZEL_TEST(codegen_fix610, compound_assign_operator_overload_610_plus_class_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Vec(X: int, Y: int) "
        "  public static method operator +(a: Vec, b: Vec): Vec => "
        "    new Vec(a.X + b.X, a.Y + b.Y); "
        "end Vec; "
        "class Program "
        "  public static method Main(): int "
        "    var v := new Vec(10, 20); "
        "    v += new Vec(1, 2); "
        "    return v.X + v.Y; "
        "  end Main; "
        "end Program;"),
        33);
}
