// Codegen — a NON-ESCAPING lambda in a loop body that captures an OUTER local
// the loop reassigns frees every iteration's capture cell (issue #8949; spec
// §27.5 / §28.9, v0.359).
//
// THE RULE (v0.359). §27.5 *Closures*: "A lambda may reference variables from
// its enclosing scope. Captured variables are heap-allocated in a closure
// object, a `ref` or `out` parameter excepted (below)." The cell is a heap
// object the compiler allocates, and it must be reclaimed exactly once
// (§28.9: the spec's own rule that every heap object is disposed exactly once).
//
// THE DEFECT (issue #8949, MEASURED on this lane's base). A non-escaping
// lambda declared in a loop body that captures an OUTER local which the same
// loop REASSIGNS leaked one capture cell per iteration but the last:
//
//   var k := 3; var s := 0;
//   for var i in 0..<5 do k := k + i; var h := lambda() => k; s := s + h(); end;
//
// 5 iterations leaked 4 cells (6400 B / 1600 allocations over 400 calls).
// Without the reassignment the same program is clean.
//
// WHY. `boxLocalSlot` reads "a store to the slot sits in the capture's own
// basic block" as "the local is re-initialised each pass" and boxes it FRESH:
// one `calloc` per iteration, each overwriting the one holder. The loop's own
// `k := k + i` is such a store, but `k` is declared OUTSIDE the loop: it is
// one variable, and only the deferred owner-scope free (which reclaims the
// holder's LAST value) ever freed a cell.
//
// THE FIX (codegen). When the captured local is declared outside the loop the
// capture sits in and the lambda is proven non-escaping, the fresh path frees
// the holder's previous cell before it is overwritten (the holder is
// null-initialised in the entry block, so pass one frees nothing). The
// previous pass's closure is gone by then, and the original slot, which this
// pass seeds from, is kept in sync, so nothing reads the freed cell.
//
// ORACLE. `runAotChecked` (checked AOT path: ASan over the generated module,
// LeakSanitizer at exit). Every row asserts the program's own output and exit
// value, so a silent zero-fill cannot pass and a too-early free would be a
// use-after-free in the ASan leg. The rows LOOP (200 calls in the program).

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
    "  public static method Run(): int "
    "    var k := 3; var s := 0; "
    "    for var i in 0..<5 do k := k + i; var h := lambda() => k; "
    "      s := s + h(); end; "
    "    return s + k; "
    "  end Run; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var j in 0..<200 do total := total + C.Run() + C.Run(); end; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end C;";

// An inner loop whose lambda captures a local of the OUTER loop's frame
// reassigned by both loops: the previous pass's cell must be freed on every
// inner pass and on every re-entry of the inner loop.
const char* kNestedLoops =
    "module M; public class C "
    "  public static method Run(): int "
    "    var k := 3; var s := 0; "
    "    for var i in 0..<4 do k := k + i; "
    "      for var j in 0..<3 do k := k + j; var h := lambda() => k + j; "
    "        s := s + h(); end; end; "
    "    return s + k; "
    "  end Run; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var j in 0..<200 do total := total + C.Run(); end; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end C;";

// The `string` capture twin: the cell holds a string VALUE, so freeing the
// previous cell must release the CELL only (the string is owned by `t`).
const char* kStringTwin =
    "module M; public class C "
    "  public static method Run(): int "
    "    var t := \"x\"; var s := 0; "
    "    for var i in 0..<4 do t := t + \"y\"; "
    "      var h := lambda() => t.Length; s := s + h(); end; "
    "    return s + t.Length; "
    "  end Run; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var j in 0..<200 do total := total + C.Run(); end; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// Row 1: the issue's repro. Pre-fix: prints 19200, then LeakSanitizer reports
// "6400 byte(s) leaked in 1600 allocation(s)". k: 3,4,6,9,13 -> s = 35, return
// 35 + 13 = 48 per call, x 400 calls.
BEZEL_TEST(codegen_loop_reassigned_outer_capture_cell_8949,
           issue_repro_every_iteration_cell_freed_spec_27_5) {
    auto r = runAotChecked(kIssueRepro);
    expectClean(r, 0, "19200\n");
}

// Row 2: nested loops (§27.5): the cell of the inner loop is freed per inner
// pass and when the inner loop is re-entered. Per call: 169 (hand-traced).
BEZEL_TEST(codegen_loop_reassigned_outer_capture_cell_8949,
           nested_loops_every_iteration_cell_freed_spec_27_5) {
    auto r = runAotChecked(kNestedLoops);
    expectClean(r, 0, "33800\n");  // 200 x 169
}

// Row 3: the string twin (§27.5 / §28.9). Per call: lengths 2+3+4+5 = 14,
// plus the final length 5 = 19.
BEZEL_TEST(codegen_loop_reassigned_outer_capture_cell_8949,
           string_capture_every_iteration_cell_freed_spec_27_5) {
    auto r = runAotChecked(kStringTwin);
    expectClean(r, 0, "3800\n");  // 200 x 19
}
