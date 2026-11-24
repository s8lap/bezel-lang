// Regression tests for #9065 (§6.6 / §9.4 / §21.1 / §23.4 / §27.5 / §28.9,
// v0.359): the INLINED expression-bodied indexer getter (and the inlined
// indexer setter) of a COMPOUND assignment form capture cells and closure
// environments that are NEVER RELEASED.
//
// THE DEFECT. `lowerIndexerCompoundCore`
// (src/codegen/LlvmBackend_Assign.cpp) INLINES both accessor bodies into the
// CALLER's frame. Two consequences, both measured in the emitted LLVM IR:
//
//  (1) The inlined body opened NO scope of its own, so the accessor's own
//      bindings were released — where they were released at all — at the
//      CALLER's scope end: once per program, not once per INVOCATION. In a
//      loop that is 199 of the 200 per-invocation capture cells never
//      released.
//  (2) The CALLER's capture pre-scan (`collectCellFreeEligible` over the
//      caller's own statement list) never sees the accessor's body — the
//      body hangs off the property declaration, not off the caller's AST —
//      so the accessor's captured bindings are in NEITHER
//      `cellFreeEligible_` NOR `closureReleaseEligible_`, and `lowerLambda`
//      records NO capture cell for them. Nothing was ever released: not the
//      closure objects, not the closure environments, not the capture cells.
//
// The #8941 fix (the commit `27cb0a687` on this staging branch) made it
// worse: it marked the getter's index parameter as a FRESH per-invocation
// binding (so that a capture of it sees THIS invocation's value, §23.4 /
// §27.5), which turns the getter's capture cell from ONE guarded cell into
// one cell PER INVOCATION — and none of them was ever released.
//
// MEASURED on the base (r30-c1 @ 5697baf74) with the issue's own program
// under LeakSanitizer: "SUMMARY: AddressSanitizer: 69596 byte(s) leaked in
// 2599 allocation(s)" over 200 compound assignments — 13 allocations per
// pass (4 closure objects, 4 closure environments, 5 capture cells). With the
// #8941 getter-arm mark removed (one line) the same program measures
// 68008 B / 2202 allocs, so #8941's share is 397 allocations / 1588 B over
// 200 passes = about 2 allocations and 8 bytes per pass.
//
// THE SPEC. §6.6 (v0.359): "when the final segment is accessor-mediated (a
// property or an indexer), its getter and setter are each invoked exactly
// once — the getter produces the left operand, the setter stores the
// result", and §9.4 (v0.359): "an author-written accessor that an assignment
// or compound assignment invokes (§6.6) is a member call like any other" —
// so the accessor's body runs as an INVOCATION, not as a piece of the
// caller's body. §21.1 (v0.359): "A local declared by `var`, `const`,
// `using var` or `await using var` is in scope from the end of its
// declaration to the end of the innermost enclosing statement block", and
// the accessor's parameters are among that block's bindings ("an indexer's
// parameters included, §23.4") — so the accessor's scope end is the end of
// THAT invocation. §27.5 (v0.359): "Captured variables are heap-allocated in
// a closure object", and §28.9 (v0.359): "Runtime management governs the
// *function value itself* — the heap-allocated closure object that backs
// it" — so every capture is a heap allocation the runtime owns and has to
// reclaim. §17.15 (v0.359): "The atom's bindings are one scope, nested in
// the scope in which the atom appears" — so the getter's binding atom ends
// with the accessor's body, not with the caller's.
//
// THE FIX. The compound path's inlined getter body opens the ACCESSOR's own
// invocation scope, runs the accessor's own capture pre-scan over it (the
// `return <expr>;` synthesis the expression-bodied METHOD already uses), and
// consumes the capture cells at the end of the body, so the scope-end frees
// land in the ACCESSOR's scope and fire at the end of EACH invocation. The
// inlined setter body gets the `AccessorClosureScope` analysis that the other
// six accessor emitters already install.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output and exit value as well as the leak count, so a
// silent zero-fill cannot pass. The rows LOOP (200 compound assignments
// inside the program, the issue's own repro), so the leak cannot hide under
// the sanitizer's noise.

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

// The issue's own program, verbatim: the reviewer row
// `reviewer_getter_then_setter_capture_200x_spec_23_4_27_5` of
// tests/codegen/inlined_indexer_compound_getter_capture_8941_test.cpp. The
// getter is an expression body whose §17.15 binding atom forms two
// lambdas, the second of which CAPTURES the indexer parameter; the setter is
// a block body whose lambda captures the indexer parameter AND `value`.
const char* kIssueRepro =
    "module M; "
    "public class H "
    "  public n: int; "
    "  public property [i: int]: int "
    "    get => (var g := lambda(): int => 3; var q := g(); "
    "      var k := lambda(): int => i + q; k()); "
    "    set => do "
    "      var g := lambda(): int => 1; var q := g(); "
    "      var k := lambda(): int => i + value + q - 1; n := n + k(); "
    "    end; "
    "  end; "
    "end H; "
    "public static class Program "
    "  public static method Main(args: Array<string>): int "
    "    using var h := new H(); var i := 0; "
    "    while i < 200 do h[i] += 1; i := i + 1; end; "
    "    Console.Write(h.n); "
    "    return 0; "
    "  end; "
    "end Program;";

// The output the issue's program prints: each pass adds `i + value + q - 1`
// to `n`, where the getter yielded `i + 3` and the compound operator added
// 1, so `value` is `i + 4` and the increment is `2i + 4`; the sum over
// i in 0..199 of (2i + 4) is 40600 (the same value the #8941 row pins).
const char* kIssueReproOutput = "40600";

// THE REGRESSION ROW: the issue's own program. RED on the base (69596 B /
// 2599 allocations leak over the 200 passes), GREEN with the fix (0 B /
// 0 allocations).
BEZEL_TEST(inlined_indexer_compound_capture_leak_9065,
           getter_then_setter_capture_200x_spec_23_4_27_5_no_leak) {
    auto r = runAotChecked(kIssueRepro);
    expectClean(r, 0, kIssueReproOutput);
}

// The issue's TWIN — the getter-only shape, which #8941's own rows already
// pin for its VALUE (20700 over 200 passes). It is the shape the issue says
// is already correct, so this row pins that the fix leaves its VALUE alone
// and that its capture cell — one guarded cell, released at the CALLER's
// scope end on the base — is released too. RED on the base (the cell
// leaks), GREEN with the fix.
BEZEL_TEST(inlined_indexer_compound_capture_leak_9065,
           getter_only_capture_200x_spec_23_4_27_5_no_leak) {
    const std::string source =
        "module M; "
        "public class H "
        "  public n: int; "
        "  public property [i: int]: int "
        "    get => (var g := lambda(): int => 3; var q := g(); "
        "      var k := lambda(): int => i + q; k()); "
        "    set => n := n + value; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); var i := 0; "
        "    while i < 200 do h[i] += 1; i := i + 1; end; "
        "    Console.Write(h.n); "
        "    return 0; "
        "  end; "
        "end Program;";
    auto r = runAotChecked(source);
    expectClean(r, 0, "20700");
}

// GREEN CONTROL (not a fail-without pin): a compound assignment whose
// accessor forms NO closure at all — the ordinary shape the fix must leave
// untouched. It is green on the base and green with the fix; it is the
// "must still admit" twin of the two rows above.
BEZEL_TEST(inlined_indexer_compound_capture_leak_9065,
           no_capture_compound_200x_control) {
    const std::string source =
        "module M; "
        "public class H "
        "  public n: int; "
        "  public property [i: int]: int "
        "    get => n; "
        "    set => n := value; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); var i := 0; "
        "    while i < 200 do h[i] += 1; i := i + 1; end; "
        "    Console.Write(h.n); "
        "    return 0; "
        "  end; "
        "end Program;";
    auto r = runAotChecked(source);
    expectClean(r, 0, "200");
}

}  // namespace
