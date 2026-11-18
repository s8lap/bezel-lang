// Codegen — a capturing local declared inside an ESCAPING lambda's body has
// its capture cell freed at that local's scope end (issue #8835; spec §27.5 /
// §28.9, v0.359).
//
// THE RULE, quoted from the spec (v0.359):
//
//   §27.5 *Closures*: "A lambda may reference variables from its enclosing
//   scope. Captured variables are heap-allocated in a closure object, a `ref`
//   or `out` parameter excepted (below)."
//
//   §28.9 *Runtime-managed types*: a function-typed value's "lifecycle ... is
//   not part of this specification" — but the heap object a capture CELL is
//   must still be reclaimed exactly once, like every other heap object the
//   compiler allocates (the spec's own rule: an owned local is auto-disposed
//   at scope end).
//
// THE DEFECT (issue #8835, MEASURED on this lane's base). A capturing local
// declared inside the body of an ESCAPING lambda (returned, or stored into a
// sink) leaks its capture cell, one cell per activation of the escaping
// lambda:
//
//   module M; public class C
//     public static method Make(): () -> int
//       return lambda() => do var k := 3; var h := lambda() => k;
//         return h(); end;
//     end Make;
//     ...
//
// prints 1200 (correct) and then LeakSanitizer reports "1600 byte(s) leaked
// in 400 allocation(s)" — the 4-byte cell for `k`, `calloc`ed in `lam$0`.
// The same `var k; var h := lambda() => k; return h();` in a METHOD frame is
// clean.
//
// WHY. The scope-end free of a capture cell ("boxedCellFree", the #429 route)
// is armed in `lowerLambda`'s captured-locals loop
// (`src/codegen/LlvmBackend_Exprs2.cpp`) under five conjuncts. Two of them
// decide the outcome for this shape:
//
//   * `cellFreeEligible_.count(decl)` — the cell may be freed at scope end
//     only if EVERY lambda that captures the local is proven NON-escaping.
//     This is the re-scanned frame's OWN classification, and it is CORRECT
//     here: `h` is a `var` local of the escaping lambda's body, so `h` never
//     escapes and `k` is free-eligible in that frame.
//
//   * `!declsCapturedByEscaping_.count(decl)` — the double-free guard
//     (#4210): if an ESCAPING lambda also captures the same local, the
//     escaping closure's envdtor frees the cell, so the scope-end free must
//     not also fire.
//
// `declsCapturedByEscaping_` is a MODULE-WIDE set, and the ENCLOSING frame's
// scan puts an entry in it for every symbol an escaping lambda's body
// mentions — INCLUDING that escaping lambda's OWN locals, which it does not
// capture at all (they are allocated in ITS frame, per activation of it). The
// enclosing frame's `collectLambdaCaptures(R, isNonEscaping=false)` walks the
// escaping lambda's WHOLE body, so it finds `R`'s own local `k` and records
// it as escaping-captured. `collectCellFreeEligible` clears
// `cellFreeEligible_` for a nested lambda's re-scan but never
// `declsCapturedByEscaping_`, so the stale entry survives into the nested
// frame's own re-scan.
//
// So BOTH routes decline: the scope-end route declines on the stale entry,
// and the #4509 refcount route declines because `cellFreeEligible_` (the
// nested frame's own, correct classification) holds the key. Nobody frees
// the cell.
//
// THE FIX (codegen). For a local declared in the frame being lowered, the
// accumulated `declsCapturedByEscaping_` set is NOT the authority: only a
// lambda lexically inside that frame can capture it (lexical scoping), and
// that frame's own scan classified every one of them — so
// `cellFreeEligible_.count(decl)` (already a conjunct of the same condition)
// already proves that no escaping lambda in that frame captures it, and the
// #4210 double-free protection is preserved by that conjunct alone. The
// accumulated-set guard is therefore applied only to a local the frame does
// NOT declare — where it stays exactly as strict as before.
//
// WHAT IS DELIBERATELY NOT CHANGED. The nested-widened shape
// (`var f2 := lambda() => lambda() => k; return f2()();` inside the escaping
// lambda) keeps its own reclaim route: the §27.5/#1135 nested-lambda widening
// classifies the inner-most lambda as escaping, so `k` is NOT free-eligible
// and the #4509 counted release reclaims the cell. That route is pinned by
// `tests/ownership/nested_lambda_capture_cell_scope_end_free_7031_test.cpp`
// rows 5-6 and is left green. A local of an ANCESTOR frame captured by an
// escaping lambda keeps the accumulated-set guard unchanged: the escaping
// closure really does hold that cell, so the scope-end free must still
// decline.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output and exit value as well as the leak count, so a
// silent zero-fill cannot pass, and a fix that freed the cell too early would
// show up as a use-after-free in the ASan leg. The rows LOOP (200 activations
// inside the program), so a one-shot leak cannot hide under the sanitizer's
// noise: at ONE activation the leaked cell stays REACHABLE from a stale stack
// slot and LSan (which reports only UNREACHABLE allocations) does not report
// it.

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

// The issue's own repro, VERBATIM (module M; class C; Make; Main). The
// escaping lambda is `Make`'s RETURN value, so it outlives `Make`'s frame;
// `k` is declared in ITS body, so `k`'s cell is allocated per activation of
// it, in its own frame.
const char* kIssueRepro =
    "module M; public class C "
    "  public static method Make(): () -> int "
    "    return lambda() => do var k := 3; "
    "      var h := lambda() => k; return h(); end; "
    "  end Make; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var j in 0..<200 do var g := C.Make(); "
    "      total := total + g() + g(); end; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end C;";

// The same shape with a `string` capture instead of an `int` one: the cell is
// an 8-byte slot, and its scope-end free must release the CELL and must NOT
// release the string it holds (the string is owned by the local `s`, which
// disposes it exactly once at ITS scope end).
const char* kStringTwin =
    "module M; public class C "
    "  public static method Make(): () -> int "
    "    return lambda() => do var s := \"hi\"; "
    "      var h := lambda() => s.Length; return h(); end; "
    "  end Make; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var j in 0..<200 do var g := C.Make(); "
    "      total := total + g() + g(); end; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end C;";

// The same shape PLUS a capture of an ANCESTOR-frame local (`x`, declared in
// `Make`, which the escaping lambda's body reads). `x`'s cell belongs to
// `Make`'s frame and the escaping closure really does hold it, so `x`'s cell
// must NOT be freed at the escaping lambda's scope end — the accumulated-set
// guard stays in force for it. This row is RED on the base for `k`'s cell
// only (the ancestor's `x` was never leaked).
const char* kAncestorCaptureToo =
    "module M; public class C "
    "  public static method Make(): () -> int "
    "    var x := 5; "
    "    return lambda() => do var k := 3; "
    "      var h := lambda() => k; return h() + x; end; "
    "  end Make; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var j in 0..<200 do var g := C.Make(); "
    "      total := total + g(); end; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// ============================================================================
// Row 1 — THE ISSUE'S OWN REPRO, verbatim (§27.5 / §28.9, v0.359).
//
// Pre-fix: prints 1200, then LeakSanitizer reports "1600 byte(s) leaked in
// 400 allocation(s)" — one 4-byte cell per activation of the escaping lambda
// (200 activations x 2 calls). Post-fix: prints 1200, rc 0, no sanitizer
// report.
//
// The row also pins the VALUE (1200): the captured `k` is read through the
// cell by the nested closure, so a fix that freed the cell too early (or
// dropped the capture) would change it — and a too-early free would also be a
// use-after-free in the ASan leg.
// ============================================================================
BEZEL_TEST(codegen_nested_escaping_lambda_capture_cell_8835,
           escaping_lambda_body_local_capture_cell_freed_spec_27_5) {
    auto r = runAotChecked(kIssueRepro);
    expectClean(r, 0, "1200\n");  // 200 x (3 + 3)
}

// ============================================================================
// Row 2 — the `string` capture twin (§27.5 / §28.9, v0.359).
//
// The capture cell of a `string` capture is an 8-byte slot (the string
// VALUE), so the cell's scope-end free must release the CELL and must NOT
// release the string it holds — the string is owned by the local `s`, which
// disposes it exactly once at ITS scope end. Pre-fix: 3200 byte(s) leaked in
// 400 allocation(s) (MEASURED on this lane's base) and nobody freed the
// cell. Post-fix: every cell is freed at the local's scope end, and the ASan
// leg proves the string is neither leaked nor released twice.
//
// The row reads `s.Length` rather than concatenating: the CONCATENATION
// result (`s + "!"`) has a SEPARATE, PRE-EXISTING leak — a string temporary
// produced inside a nested lambda and bound to a local is not released
// (MEASURED on this lane's base: 32 B direct + 4 B indirect in `lam$1`, with
// or without this fix). That defect is filed separately (#8788) and is
// deliberately NOT what this row pins; reading `.Length` keeps this row's
// leak attributable to the CAPTURE CELL alone.
// ============================================================================
BEZEL_TEST(codegen_nested_escaping_lambda_capture_cell_8835,
           escaping_lambda_body_string_capture_cell_freed_spec_27_5) {
    auto r = runAotChecked(kStringTwin);
    expectClean(r, 0, "800\n");  // 200 x (2 + 2)
}

// ============================================================================
// Row 3 — SOUNDNESS: an ANCESTOR-frame local captured by the same escaping
// lambda (§27.5 / §28.9, v0.359).
//
// `x` is declared in `Make`, so its cell belongs to `Make`'s frame and the
// escaping closure really does hold it: the scope-end free of `x`'s cell must
// NOT fire at the escaping lambda's scope end (the accumulated-set guard
// stays in force for a local the frame does not declare). A fix that dropped
// that guard would free `x`'s cell while the escaping closure still reads it
// — a use-after-free, which the ASan leg would report.
//
// Pre-fix: 800 byte(s) leaked in 200 allocation(s) — `k`'s cell only; `x`'s
// cell was never leaked. Post-fix: nothing leaks, and the value proves both
// cells were alive when they were read: k (3) + x (5) = 8 per activation.
// ============================================================================
BEZEL_TEST(codegen_nested_escaping_lambda_capture_cell_8835,
           ancestor_frame_capture_not_freed_early_spec_27_5) {
    auto r = runAotChecked(kAncestorCaptureToo);
    expectClean(r, 0, "1600\n");  // 200 x (3 + 5)
}
