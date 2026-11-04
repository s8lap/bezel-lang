// Regression tests for issue #8159: a LAMBDA whose body yields a `float` at a
// `(float) -> double` function type must widen the body's value to the
// declared result type (spec §5.1 "Implicit conversions", row "`float` |
// `double`", v0.355; §27.3 "Admission at a target-typed non-call position":
// "its inferred return type must equal the target's return slot or implicitly
// convert to it"). Before the fix the closure was emitted `float (ptr, float)`
// while the call site called it through the `(float) -> double` type as
// `double (ptr, float)`, so the f32 bits were RE-INTERPRETED as an f64 — 1.25f
// printed as 5.273905555e-315.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

void expectPrints(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// Expression body: the body's `float` value widens to the `double` return slot.
BEZEL_TEST(lambda_return_float_widen_8159, expression_lambda_float_to_double) {
    expectPrints(
        "module M; class C "
        "  public static method Main(): int "
        "    var g: (float) -> double := lambda(x: float) => x; "
        "    var f: float := 1.25f; "
        "    Console.WriteLine(g(f)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "1.25\n");
}

// Block body: the same widening on the `return x;` path.
BEZEL_TEST(lambda_return_float_widen_8159, block_lambda_float_to_double) {
    expectPrints(
        "module M; class C "
        "  public static method Main(): int "
        "    var g: (float) -> double := "
        "      lambda(x: float) => do return x; end; "
        "    var f: float := 1.25f; "
        "    Console.WriteLine(g(f)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "1.25\n");
}

// A captured `float` in the body: the body's type is still `float`, so the
// capture must not change the widening.
BEZEL_TEST(lambda_return_float_widen_8159, captured_float_to_double) {
    expectPrints(
        "module M; class C "
        "  public static method Main(): int "
        "    var c: float := 0.5f; "
        "    var g: (float) -> double := lambda(x: float) => x + c; "
        "    var f: float := 1.25f; "
        "    Console.WriteLine(g(f)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "1.75\n");
}

// `int` body at a `double` return slot (§5.1 row "`int` | ... `double` ...").
BEZEL_TEST(lambda_return_float_widen_8159, int_body_to_double) {
    expectPrints(
        "module M; class C "
        "  public static method Main(): int "
        "    var g: (int) -> double := lambda(x: int) => x; "
        "    var i: int := 3; "
        "    Console.WriteLine(g(i)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "3\n");
}

// `int` body at a `decimal` return slot (§5.1 row "`int` | ... `decimal`").
BEZEL_TEST(lambda_return_float_widen_8159, int_body_to_decimal) {
    expectPrints(
        "module M; class C "
        "  public static method Main(): int "
        "    var g: (int) -> decimal := lambda(x: int) => x; "
        "    var i: int := 3; "
        "    Console.WriteLine(g(i)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "3\n");
}

// Control: a METHOD REFERENCE at the same `(float) -> double` type already
// bridged the slot (§27.6) and must keep working.
BEZEL_TEST(lambda_return_float_widen_8159, method_reference_control) {
    expectPrints(
        "module M; class C "
        "  public static method Main(): int "
        "    var g: (float) -> double := C.Twice; "
        "    var f: float := 1.25f; "
        "    Console.WriteLine(g(f)); "
        "    return 0; "
        "  end Main; "
        "  public static method Twice(x: float): float "
        "    return x * 2.0f; "
        "  end Twice; "
        "end C;",
        "2.5\n");
}
