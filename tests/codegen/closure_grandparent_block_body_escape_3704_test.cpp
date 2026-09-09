// fix #3704 (§27.5 Closures / §15.3 Capture cells / §28.9 Runtime-managed
// types, v0.294): #1135's grandparent-cell-kept-alive protection covers the
// EXPRESSION-bodied nested-lambda-escape spelling (`lambda() => lambda() =>
// x`, pinned by codegen_closures_27_5_1135 in closure_capture_test.cpp) but
// missed one BLOCK-bodied spelling of the identical construct: a bare
// LambdaExpr literal directly in a `return` statement, with no intermediate
// `var`/`const` binding naming the escaping inner lambda.
//
// Root cause: `Emitter::collectTransparentForwardTargets`
// (LlvmBackend_Setup.cpp, the straight-line-block-body branch)'s ReturnStmt
// arm only recognized `return <identifier bound to a lambda>` — a bare
// LambdaExpr literal in `r->value` fell through to the `else` and forced
// `found` to null. That made `outer` (below) invisible as a "transparent
// forwarder", so `collectDefinitelyEscapingLambdas` could never resolve
// `return outer();` to the literal inner lambda and mark it definitely-
// escaping. The grandparent's capture cell for `x` leaked as a result —
// NOT a bug in `collectCellFreeEligible`'s `LambdaFinder` / #1135 nested-
// escape widening, which the issue's own filing flagged only as a
// candidate to check first (and which this lane's investigation ruled out).
//
// Measured on HEAD e1c1c65 (this lane's base, before the fix), 500-
// iteration loop, `runAotChecked` (generated-code ASan + LeakSanitizer):
//   - `return lambda() => x;` (no intermediate local) -- LEAKED 2000 B in
//     500 allocations (4 B/iter: one capture-cell pointer per Run() call).
//   - `var inner := lambda() => x; return inner;`      -- clean (already
//     fixed by prior #3804/#3805 forwarding work landed before this lane).
//   - `const inner := lambda() => x; return inner;`    -- clean (ditto).
//   - expression-bodied sibling `lambda() => lambda() => x`  -- clean
//     (pinned separately by codegen_closures_27_5_1135).
//
// After the fix: all four rows are leak-clean. This file pins the row that
// was still broken (the bare-literal shape) plus the three siblings as
// regression controls, so the whole #3704 grid is covered in one file.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked -- ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

const char* kHdr = "module M; import System; ";

// Common Make/Run/Main scaffold: `makeBody` supplies Make()'s statements
// after `var x := 3;`, ending in whatever produces the escaping lambda.
// Run() calls Make() then immediately invokes the escaped result, and
// Main() amplifies via 500 iterations so a per-call leak accumulates to a
// LeakSanitizer-visible total (a one-shot run can pass on both sides of a
// small per-call leak).
std::string program(const std::string& makeBody) {
    return std::string(kHdr) +
        "class C "
        "  public static method Make(): () -> int "
        "    var x := 3; " +
        makeBody +
        "  end Make; "
        "  public static method Run(): int "
        "    var f := C.Make(); "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..500 do "
        "      total := total + C.Run(); "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
}
}  // namespace

// THE FIX: a bare LambdaExpr literal directly in a block-bodied return, no
// intermediate local naming the escaping lambda. Leaked pre-fix (2000 B /
// 500 allocations); clean post-fix.
BEZEL_TEST(closure_grandparent_block_body_escape_3704,
           bare_literal_return_no_intermediate_local) {
    BEZEL_EXPECT_EQ(
        checkedOut(program(
            "var outer := lambda() => do "
            "  return lambda() => x; "
            "end; "
            "return outer(); ")),
        "total=1500\n");
}

// CONTROL: `var inner := lambda() => x; return inner;` -- already clean on
// HEAD before this fix (prior #3804/#3805 forwarding work); pinned here so
// a future regression in either code path is caught by the same file.
BEZEL_TEST(closure_grandparent_block_body_escape_3704,
           named_var_intermediate_local_regression_guard) {
    BEZEL_EXPECT_EQ(
        checkedOut(program(
            "var outer := lambda() => do "
            "  var inner := lambda() => x; "
            "  return inner; "
            "end; "
            "return outer(); ")),
        "total=1500\n");
}

// CONTROL: same shape with a spec §21.1 runtime `const` binding instead of
// `var` -- a different AST node (LocalConstDecl vs LocalDecl). Pinned
// separately per this project's LocalDecl-only-walker discipline (the
// #3591 family): a walker that only handles `ast::LocalDecl` silently
// skips every `const`, so the two forms must be exercised independently
// rather than assumed to behave identically.
BEZEL_TEST(closure_grandparent_block_body_escape_3704,
           named_const_intermediate_local_regression_guard) {
    BEZEL_EXPECT_EQ(
        checkedOut(program(
            "var outer := lambda() => do "
            "  const inner := lambda() => x; "
            "  return inner; "
            "end; "
            "return outer(); ")),
        "total=1500\n");
}

// CONTROL: the EXPRESSION-bodied sibling (`lambda() => lambda() => x`) --
// already pinned for VALUE correctness by codegen_closures_27_5_1135 in
// closure_capture_test.cpp. Repeated here under the leak-checking harness
// (runAotChecked, not the value-only runAot) so this file's own grid is
// self-contained and a future regression that only shows up as a leak
// (not a wrong value) is still caught.
BEZEL_TEST(closure_grandparent_block_body_escape_3704,
           expression_bodied_sibling_regression_guard) {
    BEZEL_EXPECT_EQ(
        checkedOut(program(
            "var outer := lambda() => lambda() => x; "
            "return outer(); ")),
        "total=1500\n");
}
