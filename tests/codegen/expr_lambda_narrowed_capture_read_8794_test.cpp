// fix #8794 (§7.4 *Narrowing inside a lambda body*, §22.6, §27.3, v0.359): an
// EXPRESSION-bodied lambda whose body is a read of a captured nullable
// value-type local that §7.4 has narrowed non-null.
//
//   §7.4 (v0.359) "a fact no form can invalidate after the lambda's formation
//   enters the body as it stands there — a `const` local's ...": the body
//   reads the captured local at its non-nullable type.
//   §22.6 (v0.359) "a read of a binding §7.4 has narrowed non-null is of its
//   non-nullable type": the payload, not the `{i8 hasValue; T}` carrier.
//   §27.3 (v0.359): the lambda's inferred return type is its body's type.
//
// Before the fix the lambda function was declared to return the CARRIER (the
// body's exprType is the declared `T?`) while every caller of the `() -> T`
// value read an `int` / `long` / `double`: `Console.WriteLine(f())` printed a
// register's garbage (measured on r30-c1 80439bb9d: 998216449, 694035201,
// 651293441 ... changing from run to run). The block-bodied spelling
// (`lambda(): int => do return k; end`) printed the payload.

#include <string>

#include "support/aot_support.hpp"   // runAot, runAotChecked
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

std::string prog(const std::string& members, const std::string& body) {
    return "module M;\n"
           "public class P\n" + members +
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end P;\n";
}
}  // namespace

// The issue's own repro.
BEZEL_TEST(codegen_expr_lambda_narrowed_capture_8794, issue_const_int_expr_body) {
    BEZEL_EXPECT_EQ(runOut(prog("",
        "    const k: int? := 5;\n"
        "    var f: () -> int := lambda() => k;\n"
        "    Console.WriteLine(f());\n")),
        std::string("5\nexit=0"));
}

// The carrier is `{i8, i64}` / `{i8, double}`: a different payload width.
BEZEL_TEST(codegen_expr_lambda_narrowed_capture_8794, long_and_double_payloads) {
    BEZEL_EXPECT_EQ(runOut(prog("",
        "    const k: long? := 5000000000L;\n"
        "    const d: double? := 2.5;\n"
        "    var f: () -> long := lambda() => k;\n"
        "    var g: () -> double := lambda() => d;\n"
        "    Console.WriteLine(f());\n"
        "    Console.WriteLine(g());\n")),
        std::string("5000000000\n2.5\nexit=0"));
}

// A narrowing by a null test (the §7.4 early-exit / branch form) rather than
// by the declaration, and the lambda passed as an argument to a `() -> int`
// formal rather than bound to a typed local.
BEZEL_TEST(codegen_expr_lambda_narrowed_capture_8794, null_test_narrowed_passed_as_argument) {
    BEZEL_EXPECT_EQ(runOut(
        prog("  static method Mk(): int? => 7;\n"
             "  static method Apply(f: () -> int): int => f();\n",
        "    const k: int? := P.Mk();\n"
        "    if k <> null then\n"
        "      Console.WriteLine(P.Apply(lambda() => k));\n"
        "    end if;\n")),
        std::string("7\nexit=0"));
}

// A lambda that is REASSIGNED: `f := lambda() => k;` takes its return slot
// from the assignment target. (The nested form `lambda() => lambda() => k`
// is the sema gap tracked as #8814.)
BEZEL_TEST(codegen_expr_lambda_narrowed_capture_8794, reassigned_lambda) {
    BEZEL_EXPECT_EQ(runOut(prog("",
        "    const k: int? := 5;\n"
        "    var f: () -> int := lambda() => 1;\n"
        "    f := lambda() => k;\n"
        "    Console.WriteLine(f());\n")),
        std::string("5\nexit=0"));
}
