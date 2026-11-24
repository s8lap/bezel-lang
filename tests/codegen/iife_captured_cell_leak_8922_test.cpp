// Codegen — issue #8922: the CAPTURE CELL of a local captured by a closure
// returned from an IIFE was never freed (4 B per call — a boxed value-type
// capture cell). The NAMED twin did not leak it.
//
// THE RULE. §28.9 (v0.359): a function-typed value is runtime-managed — "the
// type's lifetime is managed by the runtime without user involvement" — and
// runtime management governs "the heap-allocated closure object that backs
// it", so that object is reclaimed exactly once. §27.5 (v0.359): "Captured
// variables are heap-allocated in a closure object" — the captured local's
// storage is a heap cell, and that cell is reclaimed with the closure.
//
// ROOT CAUSE. The §27.5 / #1135 nested-lambda widening conservatively
// classifies an outer local referenced through a NESTED lambda as ESCAPING
// (a nested lambda may itself escape the outer one, and outer and inner share
// the SAME heap cell). fix #3805's "transparent forwarding chain" exception
// lifts that verdict for the NAMED spelling — `var g := lambda() => lambda()
// => k + 3;` — but only when the forwarder is a LOCAL, because the exception
// is keyed on `forwardTargets3804`, a map built from a local's initializer.
//
// The IMMEDIATELY-INVOKED spelling — `var f := (lambda() => lambda() => k + 3)();`
// — had no such exception: the IIFE's lambda is a TEMPORARY, not a local, so
// it is absent from `forwardTargets3804`, the nested lambda stayed classified
// ESCAPING, and the captured local was in NEITHER reclaim route:
//
//   * the scope-end route (`cellFreeEligible_`) declines a captured-by-
//     escaping local, and
//   * the envdtor route (`sharedCellEnvdtorFreeOwner_`) declines a
//     `nestedWidenedKeys` local (its condition (d)).
//
// So nobody reclaimed the 4-byte boxed cell.
//
// THE FIX. The #3805 exception now also answers for the IIFE spelling: the
// IIFE's callee is a transparent forwarder too (every evaluation constructs
// and yields exactly one nested lambda), it is called exactly once (the IIFE
// itself, at a top-level statement of the frame body — at most once per
// frame invocation), and its closure cannot escape (the IIFE path releases
// it immediately after the call). The binding that receives the result is
// proven non-escaping by the SAME guard the local-callee arm uses. So the
// nested lambda's captures may be treated as NON-ESCAPING, exactly as the
// named spelling treats them, and the cell is reclaimed at the frame's scope
// end.
//
// MEASURED on the base (r43-f45-toolchain @ 86ced0bec,
// `bezelc --sanitize=address`):
//   var f := (lambda(): () -> int => lambda() => k + 3)();
//     -> "SUMMARY: AddressSanitizer: 4 byte(s) leaked in 1 allocation(s)."
//   var g: () -> () -> int := lambda(): () -> int => lambda() => k + 3;
//   var f := g();               <- the NAMED twin: CLEAN.
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
// `Run`; `Run` returns an int, so every spelling prints its value once per
// call.
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

// THE REGRESSION ROW: the issue's repro, verbatim, in a 200× loop. The IIFE's
// result is a closure that CAPTURES the enclosing scope's local `k`.
//
// RED on the base: 4 B leaked per call (200 calls ⇒ 800 B in 200
// allocations). GREEN with the fix: prints 2600, exits 0, leaks nothing.
BEZEL_TEST(iife_captured_cell_leak_8922,
           iife_result_captures_local_spec_28_9) {
    expectClean(runAotChecked(program(
        "    var k := 10;\n"
        "    var f := (lambda(): () -> int => lambda() => k + 3)();\n"
        "    return f();\n")),
        0, "2600\n");
}

// The captured local is READ AGAIN after the IIFE — so the cell must outlive
// the closure's own use and be freed only at the frame's scope end, after
// that read. The read goes THROUGH the cell (a captured local's slot is
// rebound to it), so a fix that freed the cell too early would be a
// heap-use-after-free here.
//
// RED on the base for the same reason (the same 4 B cell). GREEN with the
// fix.
BEZEL_TEST(iife_captured_cell_leak_8922,
           captured_local_read_after_iife_spec_28_9) {
    expectClean(runAotChecked(program(
        "    var k := 10;\n"
        "    var f := (lambda(): () -> int => lambda() => k + 3)();\n"
        "    return f() + k;\n")),
        0, "4600\n");
}

// ── the two BASE-GREEN boundary controls (owner, 2026-11-12: at most two per
// suite, each named in the commit body with the wrong fix it catches) ──
//
// CONTROL 1 — the NAMED twin. The outer lambda is a LOCAL, so fix #3805's
// transparent-forwarding exception already applied and the cell was already
// reclaimed on the base. It catches the plausible wrong fix "the IIFE's
// captures are never freeable" (and any edit that disturbs the named
// spelling's own route).
BEZEL_TEST(iife_captured_cell_leak_8922,
           named_twin_spec_28_9) {
    expectClean(runAotChecked(program(
        "    var k := 10;\n"
        "    var g: () -> () -> int := lambda(): () -> int => lambda() => k + 3;\n"
        "    var f := g();\n"
        "    return f();\n")),
        0, "2600\n");
}

// CONTROL 2 — the DIRECT-capture IIFE. The IIFE's lambda captures `k`
// ITSELF (it is not a forwarder to a nested lambda), so it took the
// scope-end route on the base already. It catches the plausible wrong fix
// "the IIFE's forwarder exception is applied to every IIFE" (the direct
// capture's own route would then be disturbed) and any edit that changes the
// IIFE's own capture handling.
BEZEL_TEST(iife_captured_cell_leak_8922,
           direct_capture_iife_spec_28_9) {
    expectClean(runAotChecked(program(
        "    var k := 10;\n"
        "    var f := (lambda(): int => k + 3)();\n"
        "    return f;\n")),
        0, "2600\n");
}
