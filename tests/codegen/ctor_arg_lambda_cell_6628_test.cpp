// Regression tests for #6628: a string-capturing lambda literal passed as a
// CONSTRUCTOR argument leaks its 8-byte capture cell once the enclosing body
// declares another local.
//
// Spec (v0.359):
//  - §28.14: a function-typed parameter declared without `scoped` is
//    call-bounded: the callee holds the callback for the duration of the
//    call only, so a lambda literal passed there does not outlive the frame;
//  - §27.5: "Captured variables are heap-allocated in a closure object";
//  - §28.9: the closure object is runtime-managed and the captured local's
//    storage is reclaimed with its scope.
// The same lambda passed to a METHOD formal is clean (#3498); the constructor
// formal is the same call-bounded position (`verifiedCallBoundedFormalAt`
// covers `ast::NewExpr`, and lowerNew already released the closure itself,
// #2305). The capture-cell half was missing for constructors.
//
// Shapes: the issue's literal one-shot (the leak needs a SECOND local to
// exist, and a one-shot cannot repeat), and the looping shape that does
// repeat: the captured `string` declared INSIDE the loop body (a fresh
// variable, hence a fresh cell, per iteration, §27.5), the construction a bare
// statement.
//
// per §28.14, §27.5, §28.9, §22.7 (v0.359)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string stdoutChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return (r.ran && r.exitCode == 0) ? r.stdoutText
                                      : std::string("<did-not-run>");
}
}  // namespace

// The issue's repro shape: one-shot, a second local declared after.
BEZEL_TEST(ctor_arg_lambda_cell_6628,
           string_capturing_lambda_ctor_arg_with_later_local_spec_28_14) {
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module T;\n"
        "public class K\n"
        "  public constructor(f: () -> ()) f(); end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var s := \"hi\";\n"
        "    var k := new K(lambda() => do Console.Write(s); end);\n"
        "    var z := 1;\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n"),
        "hi");
}

// The looping shape: a fresh captured `string` per iteration.
BEZEL_TEST(ctor_arg_lambda_cell_6628,
           string_capturing_lambda_ctor_arg_released_100x_spec_28_14) {
    std::string expected;
    for (int i = 0; i < 100; ++i) expected += "hi";
    BEZEL_EXPECT_EQ(stdoutChecked(
        "module T;\n"
        "public class K\n"
        "  public constructor(f: () -> ()) f(); end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var i := 0;\n"
        "    while i < 100 do\n"
        "      var s := \"hi\";\n"
        "      new K(lambda() => do Console.Write(s); end);\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n"),
        expected);
}
