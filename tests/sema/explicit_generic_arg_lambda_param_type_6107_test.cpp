// fix #6107 (item 2; §12.7 "the target's where inferred", §14.5, v0.338):
// a lambda argument whose un-annotated parameter sits at a formal that is the
// callee's bare generic `T`, bound by an EXPLICIT type argument to a function
// type (`Pass<(string) -> int>(lambda(x) => x.Length)`), is typed against that
// function type: `x` is a `string`, and the lambda's expression type is
// `(string) -> int`.
//
// Pre-fix sema threaded nothing for a bare-`T` formal, so `x` stayed untyped
// and the lambda's expression type carried a null parameter type: a body that
// is ill-typed at the stated parameter type (`x.Length` at `int`) was accepted.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// REJECT: `x` is `int` under the explicit `(int) -> int`; `int` has no `Length`.
// Line 7, col 51 is the member access.
BEZEL_TEST(explicit_generic_arg_lambda_param_type_6107,
           untyped_param_takes_explicit_type_argument_slot_spec_12_7) {
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

// REJECT, second parameter of a two-parameter function type, block body.
BEZEL_TEST(explicit_generic_arg_lambda_param_type_6107,
           second_param_block_body_takes_explicit_type_argument_slot_spec_12_7) {
    auto r = runAot(
        "module T;\n"
        "public static class Util\n"
        "  public static method Pass<T>(v: T): Object => v;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var o := Util.Pass<(string, int) -> int>(lambda(s, n) => do return n.Length; end);\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Length` on primitive type `int`"));
}

// ACCEPT (guard, passes on base too): well-typed bodies at the explicit slot
// types still compile and run.
BEZEL_TEST(explicit_generic_arg_lambda_param_type_6107,
           well_typed_bodies_at_explicit_type_argument_still_run_spec_12_7) {
    auto r = runAot(
        "module T;\n"
        "public static class Util\n"
        "  public static method Pass<T>(v: T): Object => v;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var a := Util.Pass<(int, int) -> int>(lambda(p, q) => p + q);\n"
        "    var b := Util.Pass<(string) -> int>(lambda(x) => x.Length);\n"
        "    var c := Util.Pass<(string) -> int>(lambda(x) => do return x.Length; end);\n"
        "    Console.WriteLine(\"ok\");\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("ok") != std::string::npos);
}

// ACCEPT, value-reading: the lambda is called through `Id<T>`'s result, so the
// typed parameter and the formal's return slot decide the printed values.
// Pre-fix `x` was untyped: the block body's `return x.Length` degraded to a
// zero (#2491 warning) and both lines printed 0. `(string) -> long` over an
// `int`-valued body (§5.1 widening at the return) prints the length.
BEZEL_TEST(explicit_generic_arg_lambda_param_type_6107,
           explicit_function_type_argument_lambda_reads_typed_param_spec_12_7) {
    auto r = runAot(
        "module T;\n"
        "public static class Util\n"
        "  public static method Id<T>(v: T): T => v;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var f := Util.Id<(string) -> long>(lambda(x) => x.Length);\n"
        "    var g := Util.Id<(string) -> int>(lambda(x) => do return x.Length; end);\n"
        "    Console.WriteLine(f(\"abcd\"));\n"
        "    Console.WriteLine(g(\"ab\"));\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n2\n"));
}
