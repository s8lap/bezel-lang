// Regression test for issue #8946 — a GENERIC method returning a lambda that
// captures its `T`-typed parameter aborted (refcount over-release) or hit a
// heap-use-after-free at T = string, and leaked the capture cell at every T.
//
// THE DEFECT. `Id<T>(p: T): () -> T { return lambda() => p; }`, instantiated
// at `Program.Id<string>(s)`. The non-generic twin `Id(p: string)` is clean
// (#2689's per-capture retain, #4261-FORWARD's cell free). MEASURED on
// r30-c1 @ b2887d3d4 with `bezelc --sanitize=address`: exit 134 `refcount
// over-release (string)` with a local argument, heap-use-after-free with a
// temporary argument; int / class instantiations leaked the capture cell.
//
// THE ROOT CAUSES (a generic instantiation sees the bare `T` where the
// non-generic twin sees `string`, spec §14.1):
//  1. lowerLambda classified a captured PARAMETER by its declared type `T`,
//     so the per-capture retain / envdtor release was never keyed on it;
//  2. the `lambda() => p` result mint (+1 for a `string` / function result)
//     keys on the body's type, the bare `T`, so the lambda returned a +0
//     borrow the caller then released (the over-release);
//  3. buildSpecializedMethodFn never ran the capture-cell pre-scan that arms
//     the envdtor's cell free (collectBoxCaptureLocals /
//     collectDefinitelyEscapingLambdas), so the cell leaked at every T.
//
// THE SPEC. §27.5 (v0.359): a lambda captures the variable; §28.9 (v0.359): a
// runtime-managed value is released exactly once per reference; §14.1: a
// generic body is instantiated with the same obligations as its hand-written
// non-generic body.
//
// ORACLE. `runAotChecked` (ASan over the generated module + LeakSanitizer);
// every row loops 200 times inside the program.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor LeakSanitizer fired. `runAotChecked`
// reports an abort and any sanitizer report as `ran == false`.
void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}


// `Program.Id<T>` with the given body, a `Run` whose loop is `loop`, and an
// optional preamble type. The fixed 200-iteration loop amplifies a per-call leak.
std::string program(const std::string& idBody, const std::string& loop,
                    const std::string& preamble = "") {
    return "module P; " + preamble + "public class Program "
           "  public static method Id<T>(p: T): () -> T " + idBody +
           "  public static method Run(): int "
           "    var t := 0; "
           "    for var j in 0..<200 do " + loop + " end; "
           "    return t; "
           "  end Run; "
           "  public static method Main(): int "
           "    Console.WriteLine(Program.Run()); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

const char* const kReturnLambda = "return lambda() => p; end Id; ";

// Repro A, as the issue gives it: T = string, a LOCAL argument.
// RED on the base: exit 134, "refcount over-release (string)". GREEN with the fix.
BEZEL_TEST(generic_lambda_captures_t_param_8946,
           issue_repro_a_string_local_argument) {
    const std::string src = program(
        kReturnLambda,
        "var s := \"hi\" + \"x\"; var f := Program.Id<string>(s); "
        "t := t + f().Length; ");
    expectClean(runAotChecked(src), 0, "600\n");
}

// Repro B: T = string, a fresh TEMPORARY argument.
// RED on the base: heap-use-after-free reading the string freed earlier in Run.
BEZEL_TEST(generic_lambda_captures_t_param_8946,
           issue_repro_b_string_temporary_argument) {
    const std::string src = program(
        kReturnLambda,
        "var f := Program.Id<string>(\"hi\" + \"x\"); t := t + f().Length; ");
    expectClean(runAotChecked(src), 0, "600\n");
}

// The BLOCK-bodied lambda `lambda() => do return p; end` at T = string, read
// twice: the block twin of the +1 result mint (emitReturn's lambda arm).
// RED on the base: heap-use-after-free.
BEZEL_TEST(generic_lambda_captures_t_param_8946,
           block_bodied_lambda_string_result_is_minted) {
    const std::string src = program(
        "return lambda() => do return p; end; end Id; ",
        "var s := \"hi\" + \"x\"; var f := Program.Id<string>(s); "
        "t := t + f().Length + f().Length; ");
    expectClean(runAotChecked(src), 0, "1200\n");
}

// T = int: the 4-byte capture CELL is the only object. RED on the base
// (800 B / 200 allocations stranded in the specialization).
BEZEL_TEST(generic_lambda_captures_t_param_8946,
           int_instantiation_reclaims_the_capture_cell) {
    const std::string src = program(
        kReturnLambda, "var f := Program.Id<int>(j); t := t + f(); ");
    expectClean(runAotChecked(src), 0, "19900\n");
}

// T = class: the 8-byte capture cell leaked per call. RED on the base.
BEZEL_TEST(generic_lambda_captures_t_param_8946,
           class_instantiation_reclaims_the_capture_cell) {
    const std::string src = program(
        kReturnLambda,
        "var b := new Box(); var f := Program.Id<Box>(b); t := t + f().V; ",
        "public class Box public V: int := 3; end Box; ");
    expectClean(runAotChecked(src), 0, "600\n");
}

// The EXPRESSION-bodied spelling `=> lambda() => p` at T = string. An
// expression-bodied generic method is INLINED into its caller (it never reaches
// buildSpecializedMethodFn) and shared root cause 2. RED on the base: exit 134,
// over-release. The 8-byte capture cell of THIS spelling is a separate, bounded
// leak tracked on its own issue, so this row uses the unchecked oracle and pins
// only the over-release.
BEZEL_TEST(generic_lambda_captures_t_param_8946,
           expression_bodied_spelling_does_not_over_release) {
    const std::string src = program(
        "=> lambda() => p; ",
        "var s := \"hi\" + \"x\"; var f := Program.Id<string>(s); "
        "t := t + f().Length; ");
    const JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "600\n");
}

}  // namespace
