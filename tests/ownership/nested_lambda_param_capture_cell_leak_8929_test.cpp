// Regression test for issue #8929 — a method that RETURNS a NESTED lambda
// (`=> lambda() => lambda() => p`) whose INNER lambda captures the method's
// own PARAMETER leaked that parameter's 8-byte capture cell on every call.
//
// THE DEFECT. `public static method Ident(p: string): () -> () -> string =>
// lambda() => lambda() => p;` — the method returns a closure whose body is
// another closure, and that inner closure captures the method's `string`
// parameter `p`. ONE cell is allocated (in `Ident`'s frame, by `boxLocalSlot`
// at the OUTER lambda's capture) and TWO envs hold it: the outer env holds it
// directly, and the inner env reaches the SAME cell through the §27.5/#1135
// nested widening (one allocation, two holders). MEASURED on
// r43-f45-toolchain @ 86ced0bec with `bezelc --sanitize=address`:
//
//   Direct leak of 1600 byte(s) in 200 object(s) allocated from:
//       #1 0x000000373341 in bz$P.Program$Ident ...
//   SUMMARY: AddressSanitizer: 1600 byte(s) leaked in 200 allocation(s).
//
// The BLOCK-bodied twin (`{ return lambda() => lambda() => p; }`) leaks
// IDENTICALLY (byte-identical LSan report), so the defect is not the
// expression-vs-block axis of #8905 — it is the nested widening.
//
// THE ROOT CAUSE. Three routes can reclaim a capture cell, and all three
// decline a cell held by TWO envs:
//   * #694's envdtor route needs a SOLE holder (`singleCaptureLocals_`), and
//     `p` is counted by two envs (the outer counts it through
//     `collectCapturedSyms`' descent into the nested body, the inner counts
//     it directly);
//   * the scope-end route declines any key an ESCAPING lambda captures
//     (`cellFreeEligible_`), and the outer lambda is returned;
//   * #3426's shared-cell election needs exactly one escaping capturer node
//     and at least one NON-escaping co-capturer (`p` has two escaping
//     capturers and no non-escaping one).
// The #4509 COUNTED-cell protocol (`bz_cell_retain` / `bz_cell_release`) is
// the only route that can reclaim a cell with several holders — and for a
// captured PARAMETER the #7611 arming of it DECLINED the cell whose CONTENTS
// are also accounted for (`pContentsAlsoManaged` — a `string` parameter's
// contents are #2689's per-capture retain). Nobody freed the cell.
//
// The `int` twin of the same shape is CLEAN on the base: a value-typed
// parameter's contents are NOT managed, so the counted-cell route arms for it.
// The difference is exactly the declined conjunct.
//
// THE FIX. For a PARAMETER the contents' accounting is per ENV and does not
// depend on WHEN the cell's own memory is freed: the contents are a BORROW
// the caller owns, #2689's per-capture retain takes one reference per env
// (one per holder of the cell) and that env's envdtor drops it, BEFORE it
// drops its cell reference. So a SHARED parameter cell (two or more envs) is
// now armed for the counted-cell protocol even when its contents are also
// managed: the frame and every env hold a counted reference, and the LAST
// one out frees the cell. The #2706 mutated-shared-cell case keeps its own
// suppression and stays declined.
//
// THE SPEC. §27.5 (v0.359): "A lambda may reference variables from its
// enclosing scope. Captured variables are heap-allocated in a closure object."
// §28.9 (v0.359): the closure's lifetime is runtime-managed, so the cell it
// owns is reclaimed with the closure. The closure IS released here (the
// caller drops it at scope end) — only the CELL was stranded.
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
// (the issue's own measurement). The EXPRESSION-bodied method returns a
// nested lambda whose inner lambda captures the method's plain `string`
// parameter.
//
// RED on the base (1600 B / 200 allocations — 8 B per call): the capture cell
// is stranded. GREEN with the fix.
BEZEL_TEST(nested_lambda_param_capture_cell_leak_8929,
           issue_repro_200_iterations_reclaims_the_capture_cell) {
    const std::string src =
        "module P; public class Program "
        "  public static method Ident(p: string): () -> () -> string "
        "    => lambda() => lambda() => p; "
        "  public static method Run(): int "
        "    var s := \"hi\" + \"x\"; "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var f := Program.Ident(s); "
        "      t := t + f()().Length; "
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

// The BLOCK-bodied twin: the same nested capture in a block body. The issue
// measured it as leaking IDENTICALLY (byte-identical LSan report), so the
// defect is the nested widening and not the expression-vs-block spelling.
//
// RED on the base (1600 B / 200 allocations). GREEN with the fix.
BEZEL_TEST(nested_lambda_param_capture_cell_leak_8929,
           block_bodied_twin_reclaims_the_capture_cell) {
    const std::string src =
        "module P; public class Program "
        "  public static method Ident(p: string): () -> () -> string "
        "    return lambda() => lambda() => p; "
        "  end Ident; "
        "  public static method Run(): int "
        "    var s := \"hi\" + \"x\"; "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var f := Program.Ident(s); "
        "      t := t + f()().Length; "
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

}  // namespace
