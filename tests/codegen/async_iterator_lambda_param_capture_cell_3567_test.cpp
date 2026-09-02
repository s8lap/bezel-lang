// Codegen / leak — issue #3567 (spec §27.5 "Closures", §28.9 "Runtime-managed
// types", §30.7 "Async iterators", v0.286): the surviving CAPTURE-CELL half of
// #3553. A lambda in an ASYNC ITERATOR body that CAPTURES the method's
// parameter leaked its 4-byte §27.5 capture cell once per enumeration, after
// `narrow #3553` had closed the 40-byte closure cell and the env block that
// hangs off it.
//
//     public static async method Gen(n: int): IAsyncEnumerable<int>
//       var f := lambda(x: int) => x + n;        /* CAPTURES n */
//       yield return f(1);
//       yield return f(2);
//     end Gen;
//
// ── THIS ISSUE IS ALREADY CLOSED, BY A COMMIT THAT DID NOT NAME IT. Verified
// on r12-cycle-1 before any change: the program above, driven to full drain
// 200 times under generated-code ASan with LeakSanitizer on, is CLEAN — not
// the "800 B in 200 allocations" the issue's residue table predicts. The fix
// is `eea0ebe`, `fix #3507`, which re-established
// `collectBoxCaptureLocals` / `collectDefinitelyEscapingLambdas` /
// `collectCellFreeEligible` and their two function tokens for
// `emitAsyncIteratorMethod`'s resume fn — exactly the pair #3567 names — for a
// lambda capturing a CATCH-ARM binding. #3567's own shape, a lambda capturing
// a PARAMETER, was never pinned by that commit's test and is pinned here, so
// the two shapes cannot regress independently.
//
// ── WHY THIS FILE EXISTS ANYWAY. Rows 1 and 2 are real FAIL-WITHOUT rows, not
// tautologies; they simply fail without a commit that is already on main
// rather than without the commit that adds them. MEASURED in this lane, by
// deleting `fix #3507`'s block from `emitAsyncIteratorMethod`'s resume
// preamble and rebuilding, everything else of the current tree in place: both
// programs leak `800 byte(s) in 200 allocation(s)` — one 4-byte §27.5 capture
// cell per enumeration, exactly the residue #3567's table predicts — and both
// are clean with the block restored. Row 3 pins what abandonment still costs,
// in the ASYNC venue, so the one remaining gap here stays visible and
// measured.
//
// ── ⚠ ROW 2 IS A KNOWN-GAP PIN, NOT AN ASPIRATION. A consumer that `break`s
// out of `await for` still leaks all three §27.5 allocations per round. That
// is NOT this issue and NOT the async iterator's resume preamble: it is the
// deliberate three-line filter in `LlvmBackend_Stmts.cpp`'s yield handler
//
//     if (d.closureRelease || d.boxedCellFree ||
//         d.boxedCellFreeDeferred || d.inlineOwnedArrayCell) continue;
//
// which keeps the §27.5 reclaims OUT of the §29.7 abandonment snapshot, and it
// is issue 3541. The SYNC iterator twin of row 2's body leaks identically
// (measured: the same 10400 B / 600 allocations), which is what says the
// filter and not the async path is the cause. The sync venue's copy of this
// pin is the `abandoned_leak_is_only_the_3541_residue` row of
// tests/codegen/iter_lambda_captured_iter_format_local_unbox_3603_test.cpp;
// this is the async one. If 3541 lands, row 2 goes LSan-clean and must be
// rewritten as a clean row — a failure here is the intended signal, not a
// regression.
// per §27.5, §28.9, §29.7, §30.7 (issue #3567, issue 3541)

#include <iostream>
#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {
void dumpDiagnostics(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        std::cout << "    [diag] " << d.message << "\n";
}

std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    if (!r.ran) dumpDiagnostics(r);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

const char* kHdr =
    "module M; import System; import System.Collections.Generic; "
    "import System.Threading.Tasks; ";

// The issue's exact iterator body: a lambda capturing the method parameter.
const char* kGen =
    "class C "
    "  public static async method Gen(n: int): IAsyncEnumerable<int> "
    "    var f := lambda(x: int) => x + n; "
    "    yield return f(1); "
    "    yield return f(2); "
    "    yield return f(3); "
    "  end Gen; ";
}  // namespace

// ── Row 1: the issue's repro, driven to FULL DRAIN 200 times. Clean since
// `eea0ebe` (fix #3507); `800 B in 200 allocations` with that commit's block
// removed (measured, see the file header). The
// printed total is unaffected either way, so LSan is the ONLY oracle that sees
// this defect and this row must not be weakened to a plain value check.
// acc = sum over j in 0..<200 of ((j+1) + (j+2) + (j+3)) = 3*19900 + 1200
//     = 60900. // per §27.5, §28.9, §30.7 (issue #3567)
BEZEL_TEST(async_iterator_lambda_param_capture_cell_3567,
           lambda_capturing_a_parameter_reclaims_its_cell_on_full_drain_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) + kGen +
        "  public static async method Main(): Task<int> "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      await for var v in C.Gen(j) do acc := acc + v; end; "
        "    end; "
        "    Console.WriteLine(\"acc=\" + acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "acc=60900\n");
}

// ── Row 2: the same body THROWN OUT OF after a yield. The exception edge is
// the one the #3500 comment records as historically dangerous for these
// reclaims, and it is clean — so the arming #3507 did is not paying for itself
// with an over-release there. It also leaks 800 B / 200 with #3507's block
// removed, so this row fails without it too rather than only asserting the
// absence of a crash. acc = sum over j of ((j+1) + 1) = 20100 + 200
// = 20300. // per §29.7, §30.7 (issue #3567)
BEZEL_TEST(async_iterator_lambda_param_capture_cell_3567,
           lambda_capturing_a_parameter_survives_a_throw_after_yield_spec_30_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Gen(n: int): IAsyncEnumerable<int> "
        "    var f := lambda(x: int) => x + n; "
        "    yield return f(1); "
        "    throw new InvalidOperationException(\"pqr\"); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      try "
        "        await for var v in C.Gen(j) do acc := acc + v; end; "
        "      catch "
        "        var e: Exception => do acc := acc + 1; end "
        "      end try; "
        "    end; "
        "    Console.WriteLine(\"acc=\" + acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "acc=20300\n");
}

// ── Row 3, the KNOWN-GAP pin (issue 3541 — read the ⚠ note in the file
// header before touching this). The consumer `break`s after one element, so
// the iterator is abandoned mid-suspension and the yield handler's §29.7
// snapshot deliberately carries none of the three §27.5 reclaims. What
// LeakSanitizer reports must be EXACTLY that residue — the closure cell, its
// env block and the capture cell, one of each per abandoned round — and
// nothing else. acc = sum over j of (j+1) = 20100.
// per §29.7, §27.5 (issue #3567, issue 3541)
BEZEL_TEST(async_iterator_lambda_param_capture_cell_3567,
           abandoned_async_iterator_leak_is_only_the_3541_residue_spec_29_7) {
    JitResult r = runAotChecked(
        std::string(kHdr) + kGen +
        "  public static async method Main(): Task<int> "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      await for var v in C.Gen(j) do acc := acc + v; break; end; "
        "    end; "
        "    Console.WriteLine(\"acc=\" + acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.stdoutText, "acc=20100\n");
    BEZEL_EXPECT_FALSE(r.ran);   // LeakSanitizer fired on the 3541 residue
    std::string report;
    for (const auto& d : r.diagnostics.diagnostics()) report += d.message;
    // Three §27.5 allocations per abandoned round over 200 rounds. A crash
    // (the over-release this arming could have become) lands in the same
    // `ran == false` shape but names neither the value nor an allocation
    // count, so it fails all three assertions rather than passing this one.
    const bool onlyResidue =
        report.find("in 600 allocation(s)") != std::string::npos;
    if (!onlyResidue) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(onlyResidue);
}
