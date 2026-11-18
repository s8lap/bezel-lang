// fix #7112 (§28.9 / §27.5, v0.359): a LAMBDA WHOSE BODY IS A METHOD REFERENCE
// leaked the method-reference closure cell it mints on every call.
//
// THE DEFECT (issue #7112). A lambda whose body is a METHOD REFERENCE
// (`lambda() => d.Hi`) evaluates the method reference on every call, and the
// reference is a §28.9 runtime-managed FUNCTION-TYPED VALUE — "the heap-
// allocated closure object that backs it" — so the callee mints a fresh +1
// closure cell (a 64-byte `bz.closure` plus an 8-byte env holding the
// captured receiver) and TRANSFERS that single reference to its caller
// (#2691's transfer, not a retain). The caller therefore owns the +1 and must
// release it when the value is dropped.
//
// The caller did not. A call whose result is DISCARDED — an expression
// statement `f();`, or the §17.13 discard spelling `_ := f();` — had no
// release anywhere, so the cell leaked once per call.
//
// MEASURED on the base (r30-c1 @ cba27ac1f), the issue's own repro with the
// result discarded, 200 iterations:
//
//   Direct leak of 12800 byte(s) in 200 object(s) allocated from:
//       #0 in calloc
//       #1 in lam$0 bezel
//   Indirect leak of 1600 byte(s) in 200 object(s) allocated from:
//       #0 in calloc
//       #1 in lam$0 bezel
//
// ⇒ 64 B direct + 8 B indirect per call, allocated INSIDE `lam$0` — the issue
// #7112 signature ("64 B direct + 8 B indirect at lam$0") and its allocation
// site. The bound twin (`var g := f();`) is clean on the base: the local's
// scope-end release owns the +1 there.
//
// ⚠ THE ISSUE'S LITERAL PROGRAM IS NOT RED, AND THAT IS NOT A REGRESSION OF
// THIS ROW. Issue #7112's program ends in `Console.WriteLine(f());`, and the
// console's own unbound-owned-temporary discharge (#7803, v0.355) releases the
// closure there — so that exact program is LeakSanitizer-clean on the base.
// The DEFECT the issue names — the method-reference cell minted inside the
// lambda body, never reclaimed — is still live at every other place the
// value is dropped, and it is the same 64 B + 8 B at the same `lam$0` site.
// The issue's program is kept here as a CORPUS CANDIDATE (base-green), and
// these rows are the still-leaking spellings of the same defect.
//
// THE SPEC.
//
// §28.9 (v0.359) *Runtime-managed types*: "Runtime management governs the
// *function value itself* — the heap-allocated closure object that backs it —
// and relaxes nothing about the values the closure captures". The closure
// object backing a method reference is one of those heap objects, so it must
// be reclaimed exactly once.
//
// §27.5 (v0.359) *Closures*: "Captured variables are heap-allocated in a
// closure object, a `ref` or `out` parameter excepted (below)." The 8-byte
// env holding the captured receiver `d` is the half of the leak the issue
// counts as "indirect".
//
// §28.3.11 (v0.359) *Unbound owned temporaries* — the boundary rule the
// release follows: "Such a temporary is disposed at the **end of the full
// expression** in which it is produced, after every use of it within that
// expression has completed"; and "in an expression statement, 'end of the full
// expression' and 'end of the statement' coincide" (§21.9). §28.9 puts a
// function value on the runtime-managed tier, so the discharge the statement
// owes it is the runtime's release of that one reference, not a user-code
// `Dispose()`.
//
// ORACLE. `runAotChecked` — the checked AOT path: the program is compiled,
// linked with the generated-code AddressSanitizer, run, and LeakSanitizer
// checks the exit. Every row asserts the program's own output and exit value
// as well as the leak count, so a silent zero-fill cannot pass, and the rows
// LOOP 200 times (the issue's own measurement) so a per-call leak cannot hide
// under the sanitizer's noise. The sanitizer leg is NOT skipped: the rows
// assert `checkedToolchainSkipped` is false, so a box without clang fails the
// row instead of passing it vacuously.

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

// The issue's own repro, with the call's result DROPPED instead of printed.
//
// The receiver `d` is the CAPTURED receiver: `d.Hi` is an INSTANCE method
// reference, so the method-reference cell's env holds `d` — the 8-byte
// indirect leak of the issue's measurement.
//
// The lambda is STORED in a local, CALLED, and its result DISCARDED, 200
// times. `d.Hi` mints a fresh closure cell per call (the lambda body is
// evaluated per call), so the leak is 200 x (64 B + 8 B).
//
// The `while` loop is the issue's own 200-iteration measurement; the program
// prints one line so a silent zero-fill cannot pass.
std::string discardedInstanceMethodReferenceProgram(const char* discard) {
    return std::string(
               "module M; "
               "import System; "
               "class D "
               "  public constructor() end constructor; "
               "  public method Hi(): int return 7; end Hi; "
               "end D; "
               "class C "
               "  public static method Main(): int "
               "    var d := new D(); "
               "    var i := 0; "
               "    while i < 200 do "
               "      var f := lambda() => d.Hi; "
               "      ") +
           discard +
           "      i := i + 1; "
           "    end while; "
           "    Console.WriteLine(\"done\"); "
           "    return 0; "
           "  end Main; "
           "end C; ";
}

}  // namespace

// THE REGRESSION ROW: the issue's own repro, with the result DISCARDED as an
// expression statement. RED on the base (12800 B direct + 1600 B indirect, in
// `lam$0`), GREEN with the fix.
BEZEL_TEST(ownership_method_ref_lambda_discarded_result_7112,
           discarded_instance_method_reference_result_released) {
    auto r = runAotChecked(
        discardedInstanceMethodReferenceProgram("f();\n      "));
    expectClean(r, 0, std::string("done\n"));
}

// The §17.13 DISCARD spelling of the same shape (`_ := f();`), which the fix
// discharges at the discard point. RED on the base (the same 64 B + 8 B per
// call), GREEN with the fix.
BEZEL_TEST(ownership_method_ref_lambda_discarded_result_7112,
           discard_spelling_instance_method_reference_result_released) {
    auto r = runAotChecked(
        discardedInstanceMethodReferenceProgram("_ := f();\n      "));
    expectClean(r, 0, std::string("done\n"));
}

// The STATIC method-reference twin: `lambda() => D.SHi` mints the same kind
// of cell, with NO captured receiver (so no 8-byte env — 64 B per call).
// The issue names the method-reference producer, not the receiver; both
// spellings must release. RED on the base (12800 B in 200 allocations), GREEN
// with the fix.
BEZEL_TEST(ownership_method_ref_lambda_discarded_result_7112,
           discarded_static_method_reference_result_released) {
    const std::string source =
        "module M; "
        "import System; "
        "class D "
        "  public constructor() end constructor; "
        "  public static method SHi(): int return 7; end SHi; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 200 do "
        "      var f := lambda() => D.SHi; "
        "      f(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(\"done\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(source);
    expectClean(r, 0, std::string("done\n"));
}
