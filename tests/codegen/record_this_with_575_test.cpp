// fix #575 — `this with { … }` inside a record instance method
//
// §9.5: `r with name := v, … end` is a non-destructive copy of the record
// with named fields replaced. §22.3 / CG29: inside a record instance method
// `this` is received as a POINTER to the record value (not a struct value).
// Before the fix, lowerWith stored the pointer directly into a struct-typed
// alloca and returned ptr from a %record.T-returning function → IR verify
// failure: "Function return type does not match operand type of return inst!
//  ret ptr %0 / %record.Pt = type { i32, i32 }".

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

// §9.5 / §22.3 (issue #575): `this with X := X + dx end` inside a record
// method — single field shifted, other field untouched.
BEZEL_TEST(codegen_issue575, this_with_single_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int) "
        "  public method ShiftX(dx: int): Pt => this with X := X + dx end; "
        "end Pt; "
        "class C public static method Main(): int "
        "  var p := new Pt(3, 4); var q := p.ShiftX(10); "
        "  return q.X * 10 + q.Y; "
        "end Main; end C;"), 134);
}

// §9.5 / §22.3: the original record is NOT mutated by `this with …` (records
// are value types; `with` produces a copy).
BEZEL_TEST(codegen_issue575, this_with_does_not_mutate_source) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int) "
        "  public method ShiftX(dx: int): Pt => this with X := X + dx end; "
        "end Pt; "
        "class C public static method Main(): int "
        "  var p := new Pt(3, 4); var q := p.ShiftX(10); "
        "  return p.X * 10 + p.Y; "
        "end Main; end C;"), 34);
}

// §9.5 / §22.3: replace two fields at once inside a record method.
BEZEL_TEST(codegen_issue575, this_with_two_fields) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int) "
        "  public method Shift(dx: int, dy: int): Pt "
        "    => this with X := X + dx, Y := Y + dy end; "
        "end Pt; "
        "class C public static method Main(): int "
        "  var p := new Pt(1, 2); var q := p.Shift(10, 20); "
        "  return q.X * 100 + q.Y; "
        "end Main; end C;"), 1122);
}

// §9.5 / §22.3: replacement expression references another field from `this`
// (reads the ORIGINAL value of Y, not the updated one).
BEZEL_TEST(codegen_issue575, this_with_field_reads_other_original) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int) "
        "  public method Swap(): Pt "
        "    => this with X := Y, Y := X end; "
        "end Pt; "
        "class C public static method Main(): int "
        "  var p := new Pt(3, 7); var q := p.Swap(); "
        "  return q.X * 10 + q.Y; "
        "end Main; end C;"), 73);
}

// §9.5 / §22.3: chained calls — p.ShiftX(10).ShiftX(5).
BEZEL_TEST(codegen_issue575, this_with_chained_calls) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int) "
        "  public method ShiftX(dx: int): Pt => this with X := X + dx end; "
        "end Pt; "
        "class C public static method Main(): int "
        "  var p := new Pt(1, 4); "
        "  var q := p.ShiftX(10).ShiftX(5); "
        "  return q.X * 10 + q.Y; "
        "end Main; end C;"), 164);
}

// Regression guard: out-of-method `with` on a plain record local still works
// (the struct-value path that existed before #575). §9.5.
BEZEL_TEST(codegen_issue575, out_of_method_with_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new P(1, 2); var b := a with x := 9 end; "
        "  return b.x * 10 + b.y; "
        "end Main; end C;"), 92);
}
