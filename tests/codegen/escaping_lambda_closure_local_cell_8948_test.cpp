// Codegen — a function-typed (or any) local of an ESCAPING lambda's body,
// captured by a lambda that body returns, has its capture cell freed (issue
// #8948; spec §27.5 / §28.9, v0.359).
//
// THE RULE (v0.359). §27.5 *Closures*: "A lambda may reference variables from
// its enclosing scope. Captured variables are heap-allocated in a closure
// object, a `ref` or `out` parameter excepted (below)." The cell is a heap
// object the compiler allocates and it must be reclaimed exactly once (§28.9).
//
// THE DEFECT (issue #8948, MEASURED on this lane's base).
//
//   return lambda() => do var h := lambda() => 3;
//                          var g := lambda() => h(); return g; end;
//
// leaked `h`'s 8-byte capture cell, one per activation of the escaping lambda
// (1600 B / 200 allocations over 200 activations). The same body in a METHOD
// frame is clean.
//
// WHY. #694's sole-holder route frees a cell in the envdtor of the ONE
// definitely-escaping lambda that captures it, and reads
// `singleCaptureLocals_` for "exactly one capturer". A nested lambda's frame
// re-scans its own body (`collectBoxCaptureLocals`), which says `h` has one
// capturer (`g`); but `lowerLambda` then MERGES the ANCESTOR scan's
// `multiCaptureLocals_` back in (#4240) and erases those keys from
// `singleCaptureLocals_`. The ancestor tally counts the escaping lambda ITSELF
// as a capturer of `h` (its body mentions `h` through the nested lambda), so
// `h` reads "captured by 2" there, although it is allocated in the escaping
// lambda's own frame and never reaches that lambda's env. The merge turned the
// overcount into "shared" and the route declined.
//
// THE FIX (codegen). The ancestor's verdict is merged only for a key THIS body
// does not itself declare: only a lambda lexically inside the body can
// capture a local the body declares, and the body's own re-scan counted every
// one of them.
//
// ORACLE. `runAotChecked`; every row asserts the program's own output, and the
// rows LOOP (200 activations) so a one-shot leak cannot hide.

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

// The issue's own repro, VERBATIM.
const char* kIssueRepro =
    "module M; public class C "
    "  public static method Make(): () -> () -> int "
    "    return lambda() => do var h := lambda() => 3; "
    "      var g := lambda() => h(); return g; end; "
    "  end Make; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var j in 0..<200 do var g := C.Make(); var i := g(); "
    "      total := total + i() + i(); end; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end C;";

// The issue's second leaking shape: an int local captured by BOTH a
// non-escaping sibling (`h`) and the returned lambda (`g`), the #3426 shared
// route of the escaping lambda's own frame.
const char* kSharedCell =
    "module M; public class C "
    "  public static method Make(): () -> () -> int "
    "    return lambda() => do var k := 3; var h := lambda() => k; "
    "      var g := lambda() => k + h(); return g; end; "
    "  end Make; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var j in 0..<200 do var g := C.Make(); var i := g(); "
    "      total := total + i() + i(); end; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// Row 1: the issue's repro. Pre-fix: prints 1200, then LeakSanitizer reports
// "1600 byte(s) leaked in 200 allocation(s)". 200 x (3 + 3).
BEZEL_TEST(codegen_escaping_lambda_closure_local_cell_8948,
           issue_repro_function_typed_local_cell_freed_spec_27_5) {
    auto r = runAotChecked(kIssueRepro);
    expectClean(r, 0, "1200\n");
}

// Row 2: the shared-cell shape (§27.5). Pre-fix: 1600 B / 200 allocations.
// Per activation: i() = 3 + h() = 6, twice = 12; 200 x 12 = 2400.
BEZEL_TEST(codegen_escaping_lambda_closure_local_cell_8948,
           shared_cell_sibling_and_returned_lambda_freed_spec_27_5) {
    auto r = runAotChecked(kSharedCell);
    expectClean(r, 0, "2400\n");
}
