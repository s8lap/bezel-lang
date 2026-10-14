// fix #6190 (§27.3 "Which block-bodied lambdas return a value", v0.338):
//
//   "A body where one path returns a value and another exits without one is a
//    compile error, the same totality rule as for methods."
//
// Pre-fix sema accepted a block lambda such as
//   lambda(x: int) => do if x > 1 then return 1; end; end
// (a value return on one path, falling off the end on the other), typing it
// `(int) -> int`. The method totality rule (bodyAlwaysExits, §21.7) is reused;
// a bare `return;` beside a value `return e;` is the other spelling of the same
// mixed body.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// REJECT, the issue's literal repro: value return on one path, fall-off on the
// other. Line 4, col 14 is the `lambda` keyword.
BEZEL_TEST(lambda_mixed_return_totality_6190,
           value_return_on_one_path_fall_off_on_other_rejected_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var f := lambda(x: int) => do if x > 1 then return 1; end; end;\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":4:14: a block-bodied lambda has a path that returns a value and "
           "a path that exits without one"));
}

// REJECT, the bare-`return;` spelling: one path `return 1;`, another `return;`.
BEZEL_TEST(lambda_mixed_return_totality_6190,
           value_return_beside_bare_return_rejected_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var f := lambda(x: int) => do if x > 1 then return 1; end; return; end;\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":4:14: a block-bodied lambda has a path that returns a value and "
           "a path that exits without one"));
}

// ACCEPT (guards): every path returns a value; a procedure lambda with a bare
// `return;`; a path that throws is not "exits without one" (methods accept it);
// a non-terminating loop that returns.
BEZEL_TEST(lambda_mixed_return_totality_6190,
           total_and_procedure_lambdas_still_accepted_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var a := lambda(x: int) => do if x > 1 then return 1; else return 2; end; end;\n"
        "    var b := lambda(x: int) => do if x > 1 then return; end; end;\n"
        "    var c := lambda(x: int) => do if x > 1 then return 1; end; throw new Exception(\"no\"); end;\n"
        "    var d := lambda(x: int) => do while true do return x; end; end;\n"
        "    b(5);\n"
        "    Console.WriteLine(a(5) + c(5) + d(7));\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("9") != std::string::npos);
}
