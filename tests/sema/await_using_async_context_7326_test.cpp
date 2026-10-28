// Regression tests for issue #7326 (and its filed twin #7396) —
// `await using` OUTSIDE an `async` method or `async` lambda was NOT rejected.
//
// THE GOVERNING TEXT.
//
//   spec §28.12 (v0.345), Bezel_Language_Spec.md:7502 — "`await using` is
//   valid only inside an async method or async lambda."
//
//   §28.12 is the section that governs the `await using` BINDING. §30.2's
//   parallel sentence ("`await` is valid only inside an `async` method or
//   `async` lambda", spec §30.2, v0.345, line 8025) governs the `await`
//   EXPRESSION — a different form. The rejection therefore cites §28.12.
//
// THE DEFECT.
//
//   `await using var t := o.ToString();` in a plain (non-`async`) method
//   COMPILED (exit 0), with only the §28.9 vacuity warning. The §30.2 gate
//   on the `await` EXPRESSION never saw the form: `await using var x := …;`
//   is an `ast::LocalDecl` carrying `isAwaitUsing` (the parser's one-token
//   lookahead production), never a `UnaryExpr`, so the unary gate had no node
//   to fire on.
//
// THE ORACLE.
//
//   A rejection is pinned by the DIAGNOSTIC'S TEXT at a REAL LOCATION — never
//   by "the program did not compile" and never by an exit code. The helper
//   below requires (a) the compiler to have rejected the program at compile
//   time with a real `<path>:<line>:<col>:` location
//   (`failedAtCompileTimeWithRealLocation`, which also excludes a crash, a
//   signal, a timeout and an ASan report), and (b) the expected TEXT to
//   appear on THAT location's own line.
//
// THE CONTROLS.
//
//   The same program inside an `async Main`, inside an `async` lambda, and the
//   plain `using var` spelling in a plain method, must all KEEP COMPILING and
//   RUN — they are what proves the fix widened nothing.
//
// Cite: language spec §28.12 (v0.345); §30.2 (v0.345) for the parallel
// `await` rule; §28.9 / §28.10 for the vacuity warning the rejected program
// also emits.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot, runAotChecked
#include "test_harness.hpp"

#include <string>
#include <vector>

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Builds a program from `lines`; the 1-based line number of `lines[i]` is
// i + 1, so a row can name the line it expects the diagnostic on without
// counting newlines by hand.
std::string prog(const std::vector<std::string>& lines) {
    std::string s;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        s += lines[i];
        s += "\n";
    }
    return s;
}

// The oracle: the program was REJECTED AT COMPILE TIME, and the diagnostic
// carrying the expected TEXT sits at the expected REAL LOCATION.
//
// `failedAtCompileTimeWithRealLocation` alone is not enough (it would accept
// any diagnostic with a location, including an unrelated one); the text must
// be on THAT location's own line, so a diagnostic that merely mentions the
// right words somewhere else does not pass.
bool rejectedAt(const std::string& program, int line, int col,
                const char* text) {
    JitResult r = runAot(program);
    if (!bezel::test::failedAtCompileTimeWithRealLocation(r)) return false;
    const std::string needle =
        ":" + std::to_string(line) + ":" + std::to_string(col) + ": ";
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        std::size_t pos = 0;
        while ((pos = d.message.find(needle, pos)) != std::string::npos) {
            const std::size_t start = pos + needle.size();
            const std::size_t eol = d.message.find('\n', start);
            const std::string one =
                d.message.substr(start, eol == std::string::npos
                                          ? std::string::npos
                                          : eol - start);
            if (one.find(text) != std::string::npos) return true;
            pos += needle.size();
        }
    }
    return false;
}

// The control oracle: the program compiles, links, runs, exits 0 and prints
// exactly `expectedStdout`. The ASan leg is the memory-safety detector; the
// exit code and the stdout are what pin the behaviour.
void expectRunsClean(const std::string& program,
                     const std::string& expectedStdout) {
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(bezel::test::diagnosticMessageContains(r, "sanitizer fired"));
    BEZEL_EXPECT_FALSE(bezel::test::diagnosticMessageContains(r, "AddressSanitizer"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedStdout);
}

// The control oracle for a program that must COMPILE (it need not run).
void expectCompiles(const std::string& program) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.12's own sentence, quoted verbatim — the rejection's TEXT.
const char* kAwaitUsingRule =
    "`await using` is valid only inside an `async` method or `async` lambda "
    "(§28.12)";

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// REJECT rows — §28.12 (v0.345). The `await using` BINDING outside an async
// context is a compile error at the declaration.
// ═══════════════════════════════════════════════════════════════════════════

// The issue's own repro, verbatim in shape: a plain (non-`async`) `Main`.
// MEASURED on the pre-fix tip: the program COMPILED (exit 0) with only the
// §28.9 vacuity warning.
BEZEL_TEST(await_using_async_context_7326,
           await_using_in_a_plain_method_is_rejected_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public class Program",
        "  public static method Main()",
        "    var o := 1;",
        "    await using var t := o.ToString();",
        "  end Main;",
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(p, 5, 5, kAwaitUsingRule));
}

// The same form over a CLASS right-hand side — the shape the §28.12
// admission is written for (the scope-end `DisposeAsync` await). The rule
// does not depend on the right-hand side's kind.
BEZEL_TEST(await_using_async_context_7326,
           await_using_over_a_class_in_a_plain_method_is_rejected_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public class Res",
        "end Res;",
        "public class Program",
        "  public static method Main()",
        "    await using var r := new Res();",
        "  end Main;",
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(p, 6, 5, kAwaitUsingRule));
}

// A NON-async lambda is its own async boundary: §30.2 says the same of the
// `await` expression, and the compiler already rejects `await` there. The
// `await using` binding is governed by the same boundary.
BEZEL_TEST(await_using_async_context_7326,
           await_using_in_a_non_async_lambda_is_rejected_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public class Program",
        "  public static method Main()",
        "    var f := lambda() => do",
        "      await using var t := 1;",
        "    end;",
        "  end Main;",
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(p, 5, 7, kAwaitUsingRule));
}

// A non-async lambda INSIDE an `async` method: the enclosing method's
// async-ness does not reach the lambda — the lambda is its own boundary.
BEZEL_TEST(await_using_async_context_7326,
           await_using_in_a_non_async_lambda_in_an_async_method_is_rejected_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public class Program",
        "  public static async method Main(): Task",
        "    var f := lambda() => do",
        "      await using var t := 1;",
        "    end;",
        "  end Main;",
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(p, 5, 7, kAwaitUsingRule));
}

// ═══════════════════════════════════════════════════════════════════════════
// CONTROL rows — green on BOTH arms; they prove the fix widened nothing.
// ═══════════════════════════════════════════════════════════════════════════

// §28.12: the SAME program inside an `async Main` is legal, and still
// compiles, runs and prints.
BEZEL_TEST(await_using_async_context_7326,
           await_using_in_an_async_method_is_unchanged_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public class Program",
        "  public static async method Main(): Task",
        "    var o := 1;",
        "    await using var t := o.ToString();",
        "    Console.WriteLine($\"[{t}]\");",
        "  end Main;",
        "end Program;",
    });
    expectRunsClean(p, "[1]\n");
}

// §28.12: an `async` LAMBDA is the second admitted context.
BEZEL_TEST(await_using_async_context_7326,
           await_using_in_an_async_lambda_is_unchanged_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public class Program",
        "  public static method Main()",
        "    var f := async lambda() => do",
        "      await using var t := 1;",
        "    end;",
        "    Console.WriteLine(\"hi\");",
        "  end Main;",
        "end Program;",
    });
    expectRunsClean(p, "hi\n");
}

// §28.10: the plain `using var` spelling is a DIFFERENT form and is not
// touched by §28.12's rule — it stays legal in a plain method.
BEZEL_TEST(await_using_async_context_7326,
           plain_using_var_in_a_plain_method_is_unchanged_spec_28_10) {
    const std::string p = prog({
        "module Conf;",
        "public class Program",
        "  public static method Main()",
        "    var o := 1;",
        "    using var t := o.ToString();",
        "    Console.WriteLine($\"[{t}]\");",
        "  end Main;",
        "end Program;",
    });
    expectRunsClean(p, "[1]\n");
}
