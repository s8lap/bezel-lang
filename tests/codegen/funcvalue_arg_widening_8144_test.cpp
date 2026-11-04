// Regression tests for issue #8144: a `float` argument at a `double` parameter
// of a FUNCTION VALUE call (indirect closure call) must widen (fpext) like the
// direct-call path. spec §5.1 "Implicit conversions" table, row
// "`float` | `double`", v0.355. Before the fix the f32 operand reached the f64
// parameter and IR verification aborted.

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

// Issue repro p13: lambda stored in a local of type (double) -> double.
BEZEL_TEST(funcvalue_arg_widening_8144, lambda_local_float_to_double) {
    expectPrints(
        "module M; class C "
        "  public static method Main(): int "
        "    var r: (double) -> double := lambda(x: double) => x * 2.0; "
        "    var f: float := 1.5f; "
        "    Console.WriteLine(r(f)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "3\n");
}

// Issue repro p11: method reference value.
BEZEL_TEST(funcvalue_arg_widening_8144, method_ref_float_to_double) {
    expectPrints(
        "module M; class C "
        "  public static method Main(): int "
        "    var r: (double) -> double := Math.Abs; "
        "    var f: float := -0.5f; "
        "    Console.WriteLine(r(f)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "0.5\n");
}

// Parameter of function type.
BEZEL_TEST(funcvalue_arg_widening_8144, parameter_float_to_double) {
    expectPrints(
        "module M; class C "
        "  static method Apply(g: (double) -> double, v: float): double "
        "    return g(v); "
        "  end Apply; "
        "  public static method Main(): int "
        "    Console.WriteLine(Apply(lambda(x: double) => x + 1.0, 2.25f)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "3.25\n");
}

// Field of function type.
BEZEL_TEST(funcvalue_arg_widening_8144, field_float_to_double) {
    expectPrints(
        "module M; class C "
        "  public fn: (double) -> double := lambda(x: double) => x; "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    c.fn := lambda(x: double) => x * 4.0; "
        "    var f: float := 0.5f; "
        "    Console.WriteLine(c.fn(f)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "2\n");
}

// Two parameters, mixed float and int at (double, long).
BEZEL_TEST(funcvalue_arg_widening_8144, mixed_float_and_int_to_long) {
    expectPrints(
        "module M; class C "
        "  public static method Main(): int "
        "    var r: (double, long) -> double := "
        "      lambda(x: double, n: long) => x + double(n); "
        "    var f: float := 0.25f; var i: int := 3000000; "
        "    Console.WriteLine(r(f, i)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "3000000.25\n");
}

// int -> long control (already worked; must stay working).
BEZEL_TEST(funcvalue_arg_widening_8144, int_to_long_control) {
    expectPrints(
        "module M; class C "
        "  public static method Main(): int "
        "    var r: (long) -> long := lambda(x: long) => x + 1; "
        "    var i: int := 41; "
        "    Console.WriteLine(r(i)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "42\n");
}

// Nullable function value called with `?.`-style invocation is not a
// distinct spelling in the grammar for function values; the nullable-formal
// path (float at `double?`) is covered here instead.
BEZEL_TEST(funcvalue_arg_widening_8144, float_to_nullable_double_formal) {
    expectPrints(
        "module M; class C "
        "  public static method Main(): int "
        "    var r: (double?) -> double := lambda(x: double?) => x ?? -1.0; "
        "    var f: float := 8.5f; "
        "    Console.WriteLine(r(f)); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "8.5\n");
}
