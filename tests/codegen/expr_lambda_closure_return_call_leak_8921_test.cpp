// Codegen — issue #8921: an EXPRESSION-bodied lambda whose body is a
// CLOSURE-RETURNING CALL minted a SECOND +1 on the returned closure cell.
// The BLOCK-bodied twin was clean. The extra reference was never dropped, so
// the closure cell (64 B) leaked on every call.
//
// THE RULE. §28.9 (v0.359): a function-typed value is runtime-managed — "the
// type's lifetime is managed by the runtime without user involvement" — and
// runtime management governs "the heap-allocated closure object that backs
// it", so that object is reclaimed exactly once. §27.5 (v0.359): "Captured
// variables are heap-allocated in a closure object" — the closure object is a
// heap object, and a lambda's body mints it whoever the caller is.
//
// ROOT CAUSE. The two lambda-body return sites disagreed on their
// transfer-exclusion list. The BLOCK-bodied site (src/codegen/
// LlvmBackend_Stmts.cpp ~L4706, fix #3804) excludes
//
//     !isLambdaClosureReturnCall(rs->value.get())
//
// but the EXPRESSION-bodied site (src/codegen/LlvmBackend_Exprs2.cpp ~L4955,
// `Emitter::lowerLambda`'s expression-body emit) did NOT:
//
//     !isPlusOneClosureProducer(lam.exprBody.get()) &&
//     !isUserMethodClosureReturnCall(lam.exprBody.get()) &&
//     !returnedClosureLocalOwnsPlusOne(lam.exprBody.get())
//
// `isLambdaClosureReturnCall` (fix #2714, widened by #8870 / #8893) is exactly
// the recognizer for "this call's result already carries the callee's +1" —
// the `h()` case, where the callee is a lambda-typed LOCAL / PARAMETER or an
// immediately-invoked lambda literal. Without the exclusion the
// expression-body site RETAINED a reference the callee had already
// transferred, so the returned closure's refcount never reached zero and its
// envdtor never ran: a permanent leak of the closure header, its env and
// every capture cell. The two body forms must agree.
//
// MEASURED on the base (r43-f45-toolchain @ 86ced0bec,
// `bezelc --sanitize=address`):
//   var g: () -> () -> int := lambda(): () -> int => h();
//     -> "SUMMARY: AddressSanitizer: 64 byte(s) leaked in 1 allocation(s)."
//   var g: () -> () -> int := lambda(): () -> int => do return h(); end;
//     -> the BLOCK-bodied twin: CLEAN.
//
// Every program runs its body 200 times under runAotChecked (generated-code
// AddressSanitizer + LeakSanitizer).

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

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor the runtime's §28.9 over-release check
// fired. `runAotChecked` reports an abort and any sanitizer report as
// `ran == false`.
void didNotCrash(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("refcount over-release") == std::string::npos);
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    didNotCrash(r, exitCode, out);
    assertNoLeak(r);
}

// The body under test, run 200 times by `Main`. `runBody` is the body of
// `Run`; `Run` returns the inner closure's result, so every spelling prints
// 3 per call.
std::string program(const std::string& runBody) {
    return R"BZL(
module M;
public class P
  public static method Run(): int
)BZL" + runBody + R"BZL(
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + P.Run(); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end P;
)BZL";
}

}  // namespace

// THE REGRESSION ROW: the issue's repro, verbatim, in a 200× loop. The
// expression-bodied lambda's body is a CALL to a lambda-typed local, so the
// call's result already carries the callee's single +1.
//
// RED on the base: 64 B leaked per call (200 calls ⇒ 12800 B in 200
// allocations). GREEN with the fix: prints 600, exits 0, leaks nothing.
BEZEL_TEST(expr_lambda_closure_return_call_leak_8921,
           expression_body_call_result_spec_28_9) {
    expectClean(runAotChecked(program(
        "    var h: () -> () -> int := lambda(): () -> int => lambda() => 3;\n"
        "    var g: () -> () -> int := lambda(): () -> int => h();\n"
        "    var f := g();\n"
        "    return f();\n")),
        0, "600\n");
}

// The IIFE spelling of the same shape — the callee is a lambda LITERAL
// invoked at its own construction site (`isLambdaClosureReturnCall`'s second
// admitted callee, widened by #8870 / #8893). The issue records it as "the
// SAME defect and NOT an IIFE-recognizer defect": it leaked identically on
// the base and after #8893's fix.
//
// RED on the base for the same reason. GREEN with the fix.
BEZEL_TEST(expr_lambda_closure_return_call_leak_8921,
           iife_body_call_result_spec_28_9) {
    expectClean(runAotChecked(program(
        "    var g: () -> () -> int :=\n"
        "      lambda() => (lambda(): () -> int => lambda() => 3)();\n"
        "    var f := g();\n"
        "    return f();\n")),
        0, "600\n");
}

// ── the two BASE-GREEN boundary controls (owner, 2026-11-12: at most two per
// suite, each named in the commit body with the wrong fix it catches) ──
//
// CONTROL 1 — the BLOCK-bodied twin. The block-body return site (fix #3804)
// already carried the exclusion, so this spelling was clean on the base. It
// catches the plausible wrong fix "add the exclusion to the block-body site
// as well" (a second edit to a site that was already right, which would
// transfer a reference the block body's own mint had NOT transferred) and any
// edit that disturbs the block body's transfer.
BEZEL_TEST(expr_lambda_closure_return_call_leak_8921,
           block_body_twin_spec_28_9) {
    expectClean(runAotChecked(program(
        "    var h: () -> () -> int := lambda(): () -> int => lambda() => 3;\n"
        "    var g: () -> () -> int := lambda(): () -> int => do return h(); end;\n"
        "    var f := g();\n"
        "    return f();\n")),
        0, "600\n");
}

// CONTROL 2 — the PRODUCER twin. The expression body's value is a lambda
// LITERAL — a +1 producer whose single reference the body TRANSFERS. It was
// clean on the base. It catches the plausible wrong fix "drop the mint
// entirely for a closure-typed expression body" (the producer's reference
// would then be transferred twice — a double free) and "drop the
// `isPlusOneClosureProducer` exclusion".
BEZEL_TEST(expr_lambda_closure_return_call_leak_8921,
           producer_body_twin_spec_28_9) {
    expectClean(runAotChecked(program(
        "    var g: () -> () -> int := lambda(): () -> int => lambda() => 3;\n"
        "    var f := g();\n"
        "    return f();\n")),
        0, "600\n");
}
