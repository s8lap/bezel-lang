// Codegen fix #498 — `x++` / `x--` on a user-defined type with
// `static method operator ++` / `operator --` must dispatch to the
// overload and write the result back (per §17.6: `x++ ≡ x := T.operator++(x)`
// and §25 operator-overload resolution).
//
// Regression: before the fix, IncrementStmt unconditionally emitted
// emitCompoundOp("+", cur, 1) on the struct/class value, which is malformed IR
// (sadd.with.overflow on an aggregate / class pointer).
//
// Spec coverage: §17.6 (increment/decrement statement), §25 (operator overloads).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

// §17.6 / §25: record with operator ++ — c++ increments via overload.
// Equivalent: c := Counter.operator++(c); return c.value; → 1.
BEZEL_TEST(codegen_498, record_operator_increment_postfix) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Counter(value: int) "
        "  public static method operator ++(c: Counter): Counter => new Counter(c.value + 1); "
        "end Counter; "
        "class C public static method Main(): int "
        "  var c := new Counter(0); c++; return c.value; "
        "end Main; end C;"), 1);
}

// §17.6 / §25: record with operator -- — c-- decrements via overload.
// Equivalent: c := Counter.operator--(c); return c.value; → 9.
BEZEL_TEST(codegen_498, record_operator_decrement_postfix) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Counter(value: int) "
        "  public static method operator --(c: Counter): Counter => new Counter(c.value - 1); "
        "end Counter; "
        "class C public static method Main(): int "
        "  var c := new Counter(10); c--; return c.value; "
        "end Main; end C;"), 9);
}

// §17.6 / §25: operator ++ applied twice explicitly — value = 2.
BEZEL_TEST(codegen_498, record_operator_increment_twice) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Counter(value: int) "
        "  public static method operator ++(c: Counter): Counter => new Counter(c.value + 1); "
        "end Counter; "
        "class C public static method Main(): int "
        "  var c := new Counter(0); c++; c++; return c.value; "
        "end Main; end C;"), 2);
}

// §17.6 / §25: operator ++ in a loop — 5 iterations → value = 5.
BEZEL_TEST(codegen_498, record_operator_increment_in_loop) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Counter(value: int) "
        "  public static method operator ++(c: Counter): Counter => new Counter(c.value + 1); "
        "end Counter; "
        "class C public static method Main(): int "
        "  var c := new Counter(0); "
        "  var i := 0; "
        "  while i < 5 do "
        "    c++; i++; "
        "  end;"
        "  return c.value; "
        "end Main; end C;"), 5);
}

// §17.6 / §25: operator ++ in a loop (only Counter, no int i) — 3 iterations → value = 3.
BEZEL_TEST(codegen_498, record_operator_increment_in_loop_simple) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Counter(value: int) "
        "  public static method operator ++(c: Counter): Counter => new Counter(c.value + 1); "
        "end Counter; "
        "class C public static method Main(): int "
        "  var c := new Counter(0); "
        "  var n := 3; "
        "  while n > 0 do "
        "    c++; n := n - 1; "
        "  end;"
        "  return c.value; "
        "end Main; end C;"), 3);
}

// §17.6 / §25: operator ++ in a while loop, single iteration — value = 1.
// (Separate from the 5-iteration test to verify that 1 iteration also works.)
BEZEL_TEST(codegen_498, record_operator_increment_loop_single_iter) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Counter(value: int) "
        "  public static method operator ++(c: Counter): Counter => new Counter(c.value + 1); "
        "end Counter; "
        "class C public static method Main(): int "
        "  var c := new Counter(0); "
        "  var i := 0; "
        "  while i < 1 do "
        "    c++; i++; "
        "  end;"
        "  return c.value; "
        "end Main; end C;"), 1);
}

// §17.6 / §25: class with operator ++ — local class var, operator returns new instance.
// After c++, c.value should be 1.
BEZEL_TEST(codegen_498, class_operator_increment_local_var) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box "
        "  public value: int := 0; "
        "  public static method operator ++(b: Box): Box "
        "    var r := new Box(); r.value := b.value + 1; return r; "
        "  end; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box(); b.value := 0; b++; return b.value; "
        "end Main; end C;"), 1);
}

// CONTROL: built-in int x++ still works — must not regress.
BEZEL_TEST(codegen_498, builtin_int_increment_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 3; x++; return x; "
        "end Main; end C;"), 4);
}

// CONTROL: built-in int x-- still works — must not regress.
BEZEL_TEST(codegen_498, builtin_int_decrement_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 7; x--; return x; "
        "end Main; end C;"), 6);
}

// CONTROL: long x-- — must not regress.
BEZEL_TEST(codegen_498, builtin_long_decrement_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: long := 100L; x--; return int(x); "
        "end Main; end C;"), 99);
}
