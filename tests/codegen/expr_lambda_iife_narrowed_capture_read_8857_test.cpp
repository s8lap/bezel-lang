// fix #8857 (§27.3, §7.4 *Narrowing inside a lambda body*, §22.6, v0.359): an
// IMMEDIATELY-INVOKED lambda whose ascribed, expression body is a read of a
// captured nullable value-type local that §7.4 has narrowed non-null.
//
//   §27.3 (v0.359): a return-type ASCRIPTION is the declared return type; the
//   lambda `lambda(): int => k` returns `int`.
//   §7.4 / §22.6 (v0.359): "a read of a binding §7.4 has narrowed non-null is
//   of its non-nullable type" - the payload, not the `{i8 hasValue; T}` carrier.
//
// Before the fix the call site `(lambda(): int => k)()` declared the callee's
// return type from the body's sema type (the carrier `int?`) while the closure
// entry returned `int`: a `var` printed a register's garbage (measured on
// r30-c1 2d64c8b64: `1` for 5), and `x += (...)()` or an IIFE nested in a
// lambda body failed IR verification (`Call parameter type does not match
// function signature`, `ret { i8, i32 } %iife.ret` in an i32 function).
// rev-s69 probes q07, q07b, q02b.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string runOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) {
        std::string s = "DID NOT RUN:";
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.severity == bezel::Severity::Error) s += " | " + d.message;
        return s;
    }
    return r.stdoutText + "exit=" + std::to_string(r.exitCode);
}

std::string prog(const std::string& body) {
    return "module M;\n"
           "public class P\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end P;\n";
}
}  // namespace

// q07b: an IIFE as a typed local's initializer.
BEZEL_TEST(codegen_expr_lambda_iife_narrowed_capture_8857, local_initializer) {
    BEZEL_EXPECT_EQ(runOut(prog(
        "    const k: int? := 5;\n"
        "    var x: int := (lambda(): int => k)();\n"
        "    Console.WriteLine(x);\n")),
        std::string("5\nexit=0"));
}

// q07: the assignment right-hand side, with an int and a double payload.
BEZEL_TEST(codegen_expr_lambda_iife_narrowed_capture_8857, assignment_rhs_int_and_double) {
    BEZEL_EXPECT_EQ(runOut(prog(
        "    const k: int? := 5;\n"
        "    var x: int := 0;\n"
        "    x := (lambda(): int => k)();\n"
        "    Console.WriteLine(x);\n"
        "    const d: double? := 2.5;\n"
        "    var y: double := 0.0;\n"
        "    y := (lambda(): double => d)();\n"
        "    Console.WriteLine(y);\n")),
        std::string("5\n2.5\nexit=0"));
}

// The compound-assignment spelling: the IIFE is an operand of `+`.
BEZEL_TEST(codegen_expr_lambda_iife_narrowed_capture_8857, compound_assignment) {
    BEZEL_EXPECT_EQ(runOut(prog(
        "    const k: int? := 5;\n"
        "    var x: int := 1;\n"
        "    x += (lambda(): int => k)();\n"
        "    Console.WriteLine(x);\n")),
        std::string("6\nexit=0"));
}

// q02b and its ascribed twin: the IIFE is the body of an enclosing lambda.
BEZEL_TEST(codegen_expr_lambda_iife_narrowed_capture_8857, nested_in_lambda_body) {
    BEZEL_EXPECT_EQ(runOut(prog(
        "    const k: int? := 5;\n"
        "    var h: () -> int := lambda() => (lambda(): int => k)();\n"
        "    Console.WriteLine(h());\n"
        "    var g: () -> int := lambda(): int => (lambda(): int => k)();\n"
        "    Console.WriteLine(g());\n")),
        std::string("5\n5\nexit=0"));
}
