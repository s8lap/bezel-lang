// Regression test for issue #8905 — a NON-escaping lambda that captures a
// plain PARAMETER of an EXPRESSION-BODIED method.
//
// THE DEFECT (introduced by #8905's first attempt, rejected). #8905's fix runs
// the capture-cell pre-scan over an expression-bodied method's body, so the
// pre-scan's NON-escaping route fires for a lambda that captures a parameter
// and does NOT escape. That route registers the scope-end `boxedCellFree` in
// `disposeStack_.back()` (src/codegen/LlvmBackend_Setup.cpp,
// `consumeLastLambdaCapturedCells`) — but an arrow body is lowered with an
// EMPTY `disposeStack_`, so `.back()` was a read of an empty vector's back():
// undefined behaviour, MEASURED as a `bezelc` SIGSEGV (exit 139) on the three
// shapes below.
//
//   method Run1(p: int): int    => Apply(lambda() => p + 1);
//   method Run1(p: string): int => (lambda() => p.Length)();
//   method Show(p: int)          => Console.WriteLine(Apply(lambda() => p + 1));
//
// (backtrace: consumeLastLambdaCapturedCells <- lowerCallDispatch <- lowerCall
// <- lowerExpr <- emitFunctionBody).
//
// On the BASE (r30-c1) the same three shapes COMPILE and LEAK — the capture
// cell is stranded, 4 B per call for the `int` capture and 8 B per call for
// the `string` capture — because the base ran no pre-scan at all and therefore
// registered no free anywhere. So each row below is RED on the base BY THE
// LEAK, RED on #8905's rejected commit BY THE CRASH, and GREEN only when the
// arrow body has the disposal scope its block-bodied twin has.
//
// THE SPEC. §27.5 (v0.359): "Captured variables are heap-allocated in a
// closure object"; §28.9 (v0.359): the closure's lifetime is runtime-managed,
// so the capture cell it owns is reclaimed with it. §28.3 (v0.359): the
// values a scope owns are disposed at that scope's end.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output AND that LeakSanitizer counted nothing, so a
// silent zero-fill cannot pass. The rows LOOP (200 iterations inside the
// program), so the leak cannot hide under the sanitizer's noise.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
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

// ROW 1 — the lambda is passed to a CALL: `=> Apply(lambda() => p + 1)`.
// The lambda does not escape, so the capture cell is reclaimed at the method's
// scope end (the block-bodied twin's mechanism).
//
// RED on the base: 800 B / 200 allocations (4 B per call — the `int` capture
// cell). RED on #8905's rejected commit: `bezelc` SIGSEGV.
BEZEL_TEST(expr_body_lambda_non_escaping_capture_8905,
           lambda_passed_to_a_call_reclaims_its_capture_cell) {
    const std::string src =
        "module P; public class Program "
        "  public static method Apply(f: () -> int): int => f(); "
        "  public static method Run1(p: int): int => Apply(lambda() => p + 1); "
        "  public static method Run(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      t := t + Program.Run1(j); "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "20100\n");
}

// ROW 2 — the IIFE: `=> (lambda() => p.Length)()`. The lambda is created and
// invoked in the same expression, so it never escapes; the `string` capture
// cell is 8 B (the cell holds a pointer).
//
// RED on the base: 1600 B / 200 allocations. RED on #8905's rejected commit:
// `bezelc` SIGSEGV.
BEZEL_TEST(expr_body_lambda_non_escaping_capture_8905,
           immediately_invoked_lambda_reclaims_its_capture_cell) {
    const std::string src =
        "module P; public class Program "
        "  public static method Run1(p: string): int => (lambda() => p.Length)(); "
        "  public static method Run(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      t := t + Program.Run1(\"hi\" + \"x\"); "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "600\n");
}

// ROW 3 — the UNIT-bodied arrow: `=> Console.WriteLine(Apply(lambda() => p + 1))`.
// The body yields no value, so the arrow body's scope has nothing to return
// and its disposal is the only thing the scope is for.
//
// RED on the base: 800 B / 200 allocations. RED on #8905's rejected commit:
// `bezelc` SIGSEGV.
BEZEL_TEST(expr_body_lambda_non_escaping_capture_8905,
           unit_bodied_arrow_reclaims_the_capture_cell) {
    const std::string src =
        "module P; public class Program "
        "  public static method Apply(f: () -> int): int => f(); "
        "  public static method Show(p: int) => Console.WriteLine(Apply(lambda() => p + 1)); "
        "  public static method Run(): int "
        "    for var j in 0..<200 do "
        "      Program.Show(j); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    std::string expected;
    for (int i = 1; i <= 200; ++i) expected += std::to_string(i) + "\n";
    expected += "0\n";
    expectClean(runAotChecked(src), 0, expected);
}

}  // namespace
