// Regression test for issue #8905 — a RETURNED EXPRESSION-BODIED lambda that
// captures a plain PARAMETER of the method that returns it leaked the
// parameter's 8-byte capture cell on every call.
//
// THE DEFECT. `public static method Ident(p: string): () -> string =>
// lambda() => p;` — the method's body is a single EXPRESSION, and the closure
// it returns captures the method's own `string` parameter. The capture cell
// `boxLocalSlot` allocates for `p` was freed by NOBODY, so every call leaked
// 8 bytes. MEASURED on r30-c1 @ a40ba4c2f with `bezelc --sanitize=address`:
//
//   Direct leak of 1600 byte(s) in 200 object(s) allocated from:
//       #1 0x0000003682e1 in bz$P.Program$Ident ...
//   SUMMARY: AddressSanitizer: 1600 byte(s) leaked in 200 allocation(s).
//
// The two twins the issue names are CLEAN on the base: the BLOCK-bodied method
// `{ return lambda() => p; }` (the same capture in a block body) and the
// local-variable form `{ var q := p; return lambda() => q; }`.
//
// THE ROOT CAUSE. Every capture-cell pre-scan collector in a method's
// prologue (src/codegen/LlvmBackend_Setup.cpp) is gated on
// `ci.bodyKind == ast::MethodBodyKind::Block && ci.stmts`, so an
// EXPRESSION-bodied method ran NONE of them. `definitelyEscapingLambdas_` (the
// returned lambda is definitely escaping) and `singleCaptureLocals_` (the
// parameter is captured by exactly one env) therefore stayed empty, and
// `lowerLambda`'s captured-PARAM arm — which arms `ce.freeCellInEnvdtor` only
// when BOTH hold — did not arm. The #7611 refcounted-cell route declined too
// (a `string` parameter's cell CONTENTS are already managed by #2689's
// per-capture retain). Nobody freed the cell.
//
// THE FIX. The expression-bodied method now runs the SAME pre-scan over the
// body it would have had — a single `return <exprBody>;` statement — so the
// two spellings classify identically and the closure's envdtor frees the cell
// (the block-bodied twin's mechanism).
//
// THE SPEC. §27.5 (v0.359): "Captured variables are heap-allocated in a closure
// object"; §28.9 (v0.359): the closure's lifetime is runtime-managed, so the
// cell it owns is reclaimed when the closure is released. The closure IS
// released here (the caller drops it at scope end) — only the CELL was
// stranded.
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

// THE REGRESSION ROW: the issue's own repro, verbatim in shape, amplified 200x
// (the issue's own measurement). The EXPRESSION-bodied method returns a lambda
// that captures the method's plain `string` parameter.
//
// RED on the base (1600 B / 200 allocations — 8 B per call): the capture cell
// is stranded. GREEN with the fix.
BEZEL_TEST(expr_body_lambda_captures_param_leak_8905,
           issue_repro_200_iterations_reclaims_the_capture_cell) {
    const std::string src =
        "module P; public class Program "
        "  public static method Ident(p: string): () -> string => lambda() => p; "
        "  public static method Run(): int "
        "    var s := \"hi\" + \"x\"; "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var f := Program.Ident(s); "
        "      t := t + f().Length; "
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

// The TYPE-GENERALITY row: the same shape over an `int` parameter. The leaked
// object is the CAPTURE CELL (4 B for a value-typed capture), not the
// `string`'s heap content — a `string` capture's CONTENTS are already
// balanced by the per-capture retain / envdtor release pair, so the `string`
// row alone cannot show which object was stranded.
//
// RED on the base (800 B / 200 allocations — 4 B per call). GREEN with the fix.
BEZEL_TEST(expr_body_lambda_captures_param_leak_8905,
           int_parameter_capture_cell_is_reclaimed) {
    const std::string src =
        "module P; public class Program "
        "  public static method Ident(n: int): () -> int => lambda() => n; "
        "  public static method Run(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var f := Program.Ident(7); "
        "      t := t + f(); "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1400\n");
}

}  // namespace
