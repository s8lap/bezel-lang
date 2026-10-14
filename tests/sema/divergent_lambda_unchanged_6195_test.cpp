// fix #6107 follow-up, pending #6195 (§27.3 "Which block-bodied lambdas return a
// value", v0.338; §12.7): a block lambda whose body only DIVERGES (every path
// throws, or `while true`; `break` is not analysed, as on origin/main) and has no bare `return;` and no
// value-carrying `return e;` is classified by §27.3's "exactly when no path
// executes a value-carrying return" as `() -> ()`, but the spec also says a
// block lambda follows "the same totality rule as for methods" (L4299). #6195
// is the open spec-question. Until the owner rules, no program that compiled
// on origin/main (e1ba6b44) may be newly rejected because of the #6107
// unit fallback: these rows pin the origin/main acceptance AND run output of
// the variable / alias / argument / return / record-component shapes.
//
// The procedure-lambda classification of a body with a bare `return;` and the
// #6107 explicit-type-argument formation are controls (unchanged by this fix).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ACCEPT + RUN (origin/main: accepted, prints x. `var g: () -> int := f` of a throws-only block lambda (the #6195 repro).)
BEZEL_TEST(divergent_lambda_unchanged_6195,
           throws_only_lambda_var_to_value_fn_type_unchanged_pending_6195) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var f := lambda() => do throw new Exception(\"x\"); end;\n"
        "    var g: () -> int := f;\n"
        "    try\n"
        "      var r := g();\n"
        "    catch\n"
        "      var ex: Exception => Console.WriteLine(ex.Message)\n"
        "    end try;\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "x\n");
}

// ACCEPT + RUN (origin/main: accepted, prints a. Alias-typed variable of an if/else-all-paths-throw block lambda.)
BEZEL_TEST(divergent_lambda_unchanged_6195,
           if_else_all_throw_lambda_via_alias_var_unchanged_pending_6195) {
    auto r = runAot(
        "module T;\n"
        "type F := () -> int;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var f := lambda() => do if 1 > 0 then throw new Exception(\"a\"); else throw new Exception(\"b\"); end; end;\n"
        "    var g: F := f;\n"
        "    try\n"
        "      var r := g();\n"
        "    catch\n"
        "      var ex: Exception => Console.WriteLine(ex.Message)\n"
        "    end try;\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "a\n");
}

// ACCEPT + RUN (origin/main: accepted, prints x. Passed as an argument to a `() -> int` parameter.)
BEZEL_TEST(divergent_lambda_unchanged_6195,
           throws_only_lambda_passed_to_value_fn_param_unchanged_pending_6195) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Run(h: () -> int)\n"
        "    try\n"
        "      var r := h();\n"
        "    catch\n"
        "      var ex: Exception => Console.WriteLine(ex.Message)\n"
        "    end try;\n"
        "  end;\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var f := lambda() => do throw new Exception(\"x\"); end;\n"
        "    Run(f);\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "x\n");
}

// ACCEPT + RUN (origin/main: accepted, prints x. Returned from a method declared `() -> int`.)
BEZEL_TEST(divergent_lambda_unchanged_6195,
           throws_only_lambda_returned_from_value_fn_method_unchanged_pending_6195) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Make(): () -> int\n"
        "    var f := lambda() => do throw new Exception(\"x\"); end;\n"
        "    return f;\n"
        "  end;\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var h := Make();\n"
        "    try\n"
        "      var r := h();\n"
        "    catch\n"
        "      var ex: Exception => Console.WriteLine(ex.Message)\n"
        "    end try;\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "x\n");
}

// ACCEPT + RUN (origin/main: accepted. `while true` diverging body, never invoked, to `() -> int` variable.)
BEZEL_TEST(divergent_lambda_unchanged_6195,
           while_true_lambda_var_to_value_fn_type_unchanged_pending_6195) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var f := lambda() => do while true do end; end;\n"
        "    var g: () -> int := f;\n"
        "    Console.WriteLine(\"ok\");\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// ACCEPT + RUN (origin/main: accepted, prints a. Stored in a record component of type `() -> int`.)
BEZEL_TEST(divergent_lambda_unchanged_6195,
           if_else_all_throw_lambda_in_record_component_unchanged_pending_6195) {
    auto r = runAot(
        "module T;\n"
        "record Rec(F: () -> int);\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var f := lambda() => do if 1 > 0 then throw new Exception(\"a\"); else throw new Exception(\"b\"); end; end;\n"
        "    var c := new Rec(f);\n"
        "    try\n"
        "      var r := c.F();\n"
        "    catch\n"
        "      var ex: Exception => Console.WriteLine(ex.Message)\n"
        "    end try;\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "a\n");
}

// CONTROL (#6107 item 1 still in force): a body with a bare `return;` path is a
// procedure lambda, `() -> ()`, and is rejected at a `() -> int` slot.
BEZEL_TEST(divergent_lambda_unchanged_6195,
           bare_return_path_lambda_still_procedure_lambda_rejected_at_value_fn_type_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var w := lambda() => do if 1 > 0 then return; else throw new Exception(\"x\"); end; end;\n"
        "    var f: () -> int := w;\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":5:25: the initializer for `f` has a type that is not implicitly "
           "convertible to the target type"));
}

// CONTROL (#6107 item 2 still in force): a lambda at an explicit function-typed
// type argument is formed at that type (§12.7).
BEZEL_TEST(divergent_lambda_unchanged_6195,
           explicit_function_typed_type_argument_still_forms_lambda_at_that_type_spec_12_7) {
    auto r = runAot(
        "module T;\n"
        "public static class Util\n"
        "  public static method Pass<T>(v: T): Object => v;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var o := Util.Pass<(int) -> int>(lambda(x) => x.Length);\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":7:51: no member `Length` on primitive type `int`"));
}

// REJECT (origin/main: rejected at compile time; staging wrongly accepted.) A
// throws-only block lambda bound to a variable and assigned to a `() -> ()`
// target is rejected on origin/main; the open #6195 question is its
// classification, so this row pins origin's rejection, not a settled §27.3 reading.
BEZEL_TEST(divergent_lambda_unchanged_6195,
           throws_only_lambda_var_to_unit_fn_type_rejected_pending_6195) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var h := lambda() => do throw new Exception(\"x\"); end;\n"
        "    var g: () -> () := h;\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":5:24: the initializer for `g` has a type that is not implicitly "
           "convertible to the target type"));
}

// ACCEPT + RUN (origin/main: overload resolution picks the `() -> int` overload
// for a throws-only lambda; staging printed `unit`.) The callback is never
// invoked, so the throw does not run.
BEZEL_TEST(divergent_lambda_unchanged_6195,
           throws_only_lambda_overload_picks_value_fn_param_unchanged_pending_6195) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Over(f: () -> int): int\n"
        "    Console.WriteLine(\"int\");\n"
        "    return 0;\n"
        "  end;\n"
        "  public static method Over(f: () -> ())\n"
        "    Console.WriteLine(\"unit\");\n"
        "  end;\n"
        "  public static method Main(args: Array<string>): int\n"
        "    Over(lambda() => do throw new Exception(\"x\"); end);\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "int\n");
}
