// Ownership — a nested lambda's capture cell is freed at the frame's scope end
// when the nested closure cannot outlive the frame (issue #7031; spec §27.5 /
// §28.9, v0.359).
//
// THE RULE, quoted from the spec (v0.359):
//
//   §27.5 *Closures*: "A lambda may reference variables from its enclosing
//   scope. Captured variables are heap-allocated in a closure object, a `ref`
//   or `out` parameter excepted (below)."
//
//   §28.9 *Runtime-managed types*: a function-typed value's "lifecycle ...
//   is not part of this specification" — but the heap object a capture cell
//   IS must still be reclaimed exactly once, like every other heap object
//   the compiler allocates (the spec's own rule, §28.9's discharge rows:
//   an owned local is auto-disposed at scope end).
//
// THE DEFECT (issue #7031, MEASURED on this lane's base). The issue's own
// repro
//
//   module P; public class Program public static method Main(): int
//     var k := 5; var f2 := lambda() => lambda() => k + 1;
//     Console.WriteLine(f2()()); return 0; end Main; end Program;
//
// prints 6 and then LeakSanitizer reports "Direct leak of 4 byte(s) in 1
// object(s)", allocated by `calloc` in `main` — the capture CELL for `k`.
//
// WHY. A capture cell is heap-allocated by the frame that DECLARES the
// captured local, and it is reclaimed at that frame's scope end (the #429
// "boxedCellFree" route) UNLESS a closure that outlives the frame holds a
// pointer to it. Three routes exist (see `sharedCellEnvdtorFreeOwner_`'s own
// declaration in `LlvmBackend_impl.hpp`): the scope-end route, the envdtor
// route (#694), and #3426's shared-cell route. For this shape the nested
// lambda was classified ESCAPING by the §27.5/#1135 nested-lambda widening
// (a nested lambda is conservatively escaping), which BLOCKS the scope-end
// route — while #3426's route declines it, because the nested lambda does
// not escape the FUNCTION (it is consumed inside `Main`, so it is not in
// `definitelyEscapingLambdas_`). Nobody reclaimed the cell.
//
// THE FIX (codegen). A nested lambda that is the whole expression body of a
// FORWARDING lambda, whose every call has its result consumed AT ONCE inside
// the frame (`f2(...)(...)` — the call site releases the temporary closure
// right after the call), cannot outlive the frame. Such a nested lambda is
// therefore reclassified NON-escaping, and the frame's scope end frees the
// cell. The proof is the #4525 "the result is consumed locally" walk, applied
// at the frame level, plus the #3804/#3805 multiplicity guard (EXACTLY ONE
// such call) and the ClosureEscapeWalker guard (the forwarding local is
// never read in an escaping position, and never referenced inside a nested
// lambda body — a reference there would create the nested closure AFTER the
// frame's scope end).
//
// WHAT IS DELIBERATELY NOT CHANGED. A forwarding local called TWICE keeps
// the conservative leak: two calls build two nested closures over the SAME
// cell, and the accepted trade-off (pinned by
// `tests/codegen/deeply_nested_escaping_lambda_capture_cell_3804_test.cpp`
// row 3) keeps that case unfixed rather than risk a double-free. The
// #3804 rows 1–2 (a nested lambda that genuinely ESCAPES the function) keep
// their own route — the envdtor route — and stay clean.
//
// LEAK AMPLIFICATION (§lsan-one-shot-leak-tests-pass-vacuously): a ONE-SHOT
// leak run can hide the defect — the leaked cell stays REACHABLE from a
// stale stack slot at exit, and LSan reports only unreachable memory. The
// same program run twice, with the second run's cell unreachable, is
// reported; the amplified rows below therefore run the frame 200 times, so
// the leak is 200 unreachable cells (800 B) rather than one reachable one.
//
// This is ownership/disposal work, so every row runs under `runAotChecked`
// (generated-code AddressSanitizer + LeakSanitizer at exit).

#include <string>

#include "support/aot_support.hpp"         // runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The issue's own repro, VERBATIM (module P; class Program; Main).
const char* const kIssueRepro =
    "module P; public class Program public static method Main(): int "
    "var k := 5; var f2 := lambda() => lambda() => k + 1; "
    "Console.WriteLine(f2()()); return 0; end Main; end Program;";

// The same shape, amplified: the frame (Run) is entered 200 times, so the
// per-invocation capture cell is allocated 200 times. Pre-fix: 200 leaked
// 4-byte cells (800 B) — LSan sees them all, because only the LAST one's
// pointer survives on the stack. Post-fix: none.
std::string amplified(const std::string& runBody) {
    return
        "module M; public class C "
        "  public static method Run(): int " + runBody +
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Run(); end; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

}  // namespace

// ============================================================================
// Row 1 — THE ISSUE'S OWN REPRO, verbatim (§27.5, v0.359).
//
// Pre-fix: prints 6, then LeakSanitizer reports "Direct leak of 4 byte(s) in
// 1 object(s)" allocated in `main`; the runAotChecked oracle flips `ran` to
// false and the process exit status to 1. Post-fix: prints 6, rc 0, no
// sanitizer report.
//
// The row also pins the VALUE (6) — the captured `k` is read through the
// cell by the nested closure, so a fix that freed the cell too early (or
// dropped the capture) would change it.
// ============================================================================
BEZEL_TEST(ownership_nested_lambda_capture_cell_7031,
           issue_repro_nested_lambda_capture_cell_freed_spec_27_5) {
    auto r = runAotChecked(kIssueRepro);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "6\n");
}

// ============================================================================
// Row 2 — the same shape, AMPLIFIED x200 (§27.5 / §28.9, v0.359).
//
// Pre-fix: 200 leaked 4-byte capture cells (800 B / 200 allocations) — one
// per Run invocation, every one of them unreachable at exit. Post-fix: the
// frame's scope end frees each cell; nothing leaks.
// ============================================================================
BEZEL_TEST(ownership_nested_lambda_capture_cell_7031,
           nested_lambda_capture_cell_amplified_200x_spec_27_5) {
    auto r = runAotChecked(amplified(
        "    var k := 5; "
        "    var f2 := lambda() => lambda() => k + 1; "
        "    return f2()(); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1200\n");  // 200 * 6
}

// ============================================================================
// Row 3 — the `string` capture twin (§27.5 / §28.9, v0.359).
//
// The capture cell of a `string` capture is an 8-byte slot (the string
// VALUE), so the cell's scope-end free must release the CELL and must NOT
// release the string it holds — the string is owned by the local `s`, which
// disposes it exactly once at ITS scope end. Pre-fix: 200 leaked 8-byte
// cells (1600 B / 200 allocations — MEASURED on this lane's base), and
// nobody freed the cell. Post-fix: every cell is freed at the frame's scope
// end, and the ASan leg proves the string is neither leaked nor released
// twice (no double-free report).
//
// The inner lambda reads `s.Length` rather than concatenating: the
// CONCATENATION result (`s + "!"`) has a SEPARATE, PRE-EXISTING leak — a
// string temporary produced inside a nested lambda and bound to a local is
// not released (MEASURED on this lane's base: 32 B direct + 4 B indirect in
// `lam$1`, with or without this fix). That defect is filed separately
// (#8788) and is deliberately NOT what this row pins; reading `.Length`
// keeps this row's leak attributable to the CAPTURE CELL alone.
// ============================================================================
BEZEL_TEST(ownership_nested_lambda_capture_cell_7031,
           nested_lambda_string_capture_cell_amplified_200x_spec_27_5) {
    auto r = runAotChecked(amplified(
        "    var s := \"hi\"; "
        "    var f2 := lambda() => lambda() => s.Length; "
        "    return f2()(); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "400\n");  // 200 * 2
}

// ============================================================================
// Row 4 — SOUNDNESS: the captured local is read AFTER the nested closure died
// (§27.5 / §28.9, v0.359).
//
// The cell must not be freed when the NESTED closure is released — only at
// the frame's own scope end, which is after this read. A fix that freed the
// cell in the nested closure's envdtor (the #3426 route) would turn this row
// into a heap-use-after-free; a fix that dropped the capture would change
// the value.
//
// AMPLIFIED x200, and that is load-bearing, not decoration: at ONE
// iteration the leaked cell stays REACHABLE from a stale stack slot and
// LSan (which reports only UNREACHABLE allocations) does not report it —
// MEASURED: the one-shot twin of this row is GREEN on the base, so it would
// be a vacuous regression row. At 200 iterations every cell but the last is
// unreachable, so the base is RED (800 B / 200 allocations).
//
// Post-fix: rc 0, and the value proves the cell was still alive when read —
// `seen` (5, read through the cell after the nested closure died) + `r` (6,
// the nested closure's result) = 11 per iteration, 2200 in total.
// ============================================================================
BEZEL_TEST(ownership_nested_lambda_capture_cell_7031,
           captured_local_read_after_nested_closure_no_uaf_spec_27_5) {
    auto r = runAotChecked(amplified(
        "    var k := 5; "
        "    var f2 := lambda() => lambda() => k + 1; "
        "    var r := f2()(); "
        "    var seen := k; "
        "    return seen + r; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2200\n");  // 200 * (5 + 6)
}
