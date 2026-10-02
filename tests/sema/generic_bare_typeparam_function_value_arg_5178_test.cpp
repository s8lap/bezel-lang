// Spec-derived tests for fix #5178 — §14.5 phase 1, v0.313.
//
// §14.5 phase 1: "Each argument whose type is self-determined — every non-
// function-typed argument, a function-typed *value*, and a lambda's annotated
// parameter types — is unified structurally against its parameter type: a
// parameter type that is a bare type parameter `X` binds `X` to the argument's
// static type." So `s.Apply(f)` against `method Apply<U>(x: U)` with a local
// `var f: () -> int` binds `U := () -> int` and is ACCEPTED, void or non-void,
// instance or static. The compiler rejected it with the §5.1 "not implicitly
// convertible to the target type" diagnostic (checkAssignable's
// function-to-non-function arm did not exempt the callee's own free formal).
//
// A lambda LITERAL and a method reference are phase 2 (§14.5 / §27.6) and are
// NOT self-determined: the controls below pin that they stay rejected against
// a bare `U`. (Rows for the value form were removed from fix #5162's test file
// rather than pinned; this file owns them.)
//
// Oracle: runAot for accepts (value assertions on stdout); rejects use
// failedAtCompileTimeWithRealLocation plus the specific diagnostic.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

BEZEL_TEST(generic_bare_typeparam_function_value_arg_5178,
           void_instance_method_infers_function_value) {
    auto r = runAot(
        "module M; "
        "class Sink5178a "
        "  public method Apply<U>(x: U) "
        "    Console.WriteLine(\"applied\"); "
        "  end Apply; "
        "end Sink5178a; "
        "public static class Program5178a "
        "  public static method Main(): int "
        "    var s := new Sink5178a(); "
        "    var f: () -> int := lambda () => 42; "
        "    s.Apply(f); "
        "    Console.WriteLine(f().ToString()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "applied\n42\n");
}

BEZEL_TEST(generic_bare_typeparam_function_value_arg_5178,
           non_void_instance_method_infers_function_value) {
    auto r = runAot(
        "module M; "
        "class Sink5178b "
        "  public method Apply<U>(x: U): int "
        "    return 7; "
        "  end Apply; "
        "end Sink5178b; "
        "public static class Program5178b "
        "  public static method Main(): int "
        "    var s := new Sink5178b(); "
        "    var f: () -> int := lambda () => 1; "
        "    Console.WriteLine(s.Apply(f).ToString()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// Generic static method returning U: U binds to the function type, the result
// is callable and yields the original function's value.
BEZEL_TEST(generic_bare_typeparam_function_value_arg_5178,
           static_generic_method_returns_bound_function_value) {
    auto r = runAot(
        "module M; "
        "public static class Util5178c "
        "  public static method Id<U>(x: U): U "
        "    return x; "
        "  end Id; "
        "end Util5178c; "
        "public static class Program5178c "
        "  public static method Main(): int "
        "    var f: (int) -> int := lambda (n: int) => n * 3; "
        "    var g := Util5178c.Id(f); "
        "    Console.WriteLine(g(14).ToString()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// Static void method; the function value is a parameter of the caller.
BEZEL_TEST(generic_bare_typeparam_function_value_arg_5178,
           static_void_method_infers_function_parameter_value) {
    auto r = runAot(
        "module M; "
        "public static class Util5178d "
        "  public static method Take<U>(x: U) "
        "    Console.WriteLine(\"took\"); "
        "  end Take; "
        "end Util5178d; "
        "public static class Program5178d "
        "  public static method Run(f: () -> int): int "
        "    Util5178d.Take(f); "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var h: () -> int := lambda () => 5; "
        "    Console.WriteLine(Run(h).ToString()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "took\n5\n");
}

// CONTROL (phase 2): a lambda LITERAL against a bare `U` is not self-
// determined (§14.5 phase 2) and stays rejected.
BEZEL_TEST(generic_bare_typeparam_function_value_arg_5178,
           lambda_literal_against_bare_typeparam_still_rejected) {
    auto r = runAot(
        "module M; "
        "class Sink5178e "
        "  public method Apply<U>(x: U) "
        "  end Apply; "
        "end Sink5178e; "
        "public static class Program5178e "
        "  public static method Main(): int "
        "    var s := new Sink5178e(); "
        "    s.Apply(lambda () => 42); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// CONTROL: an explicit non-function type argument against a function value is
// a real §5.1 mismatch and stays rejected (the exemption is only for the
// callee's own still-free formal).
BEZEL_TEST(generic_bare_typeparam_function_value_arg_5178,
           explicit_nonfunction_type_arg_with_function_value_still_rejected) {
    auto r = runAot(
        "module M; "
        "class Sink5178f "
        "  public method Apply<U>(x: U) "
        "  end Apply; "
        "end Sink5178f; "
        "public static class Program5178f "
        "  public static method Main(): int "
        "    var s := new Sink5178f(); "
        "    var f: () -> int := lambda () => 42; "
        "    s.Apply<int>(f); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type"));
}

// CONTROL: a function value against a CONCRETE (non-generic) non-function
// parameter stays rejected.
BEZEL_TEST(generic_bare_typeparam_function_value_arg_5178,
           function_value_against_concrete_int_param_still_rejected) {
    auto r = runAot(
        "module M; "
        "public static class Program5178g "
        "  public static method Take(x: int) "
        "  end Take; "
        "  public static method Main(): int "
        "    var f: () -> int := lambda () => 42; "
        "    Take(f); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type"));
}
