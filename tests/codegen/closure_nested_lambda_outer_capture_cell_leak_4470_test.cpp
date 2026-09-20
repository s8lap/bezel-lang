// fix #4470 (§27.5 / §28.9 / §28.14, v0.298): a nested lambda at the tail of a
// BLOCK-bodied outer lambda whose body declares a local (`var y := 2`) and
// returns another lambda that captures BOTH the outer-scope variable `x` AND
// the inner-block-scope `y` leaked `y`'s 4-byte capture cell under ASan/LSan.
//
// Root cause: the SHARED-CELL envdtor route (`isSharedCellEnvdtorFreeOwner`,
// fix #3426) was the missing reclaim path. For the canonical
// `var outer := lambda() => lambda() => x; return outer();` shape, the
// #3804 transparent-forwarder widening attributes the escaping nested
// lambda's captures to the forwarder (whose envdtor is the cell's sole
// reclaim path, after every non-escaping co-capturer's scope-end release)
// and `sharedCellEnvdtorFreeOwner_` registers the (outer_cell, inner)
// ownership. The BLOCK-body variant
// (`var f := lambda() => do var y; return lambda() => x + y; end;
//  var g := f(); var v := g();`) had no equivalent widening: the inner
// lambda was classified as NON-escaping (its lifetime is bounded by `g`,
// which is in `closureReleaseEligible_`), so its captures were recorded
// ONLY in `capturedByNonEscaping`, leaving `escapingCapturersOf` empty for
// the relevant keys. `sharedCellEnvdtorFreeOwner_` then declined via
// condition (a) (`lams.size() != 1`), and the cell was reclaimed by no
// route.
//
// Measured on HEAD e1733e53 (this lane's base, before the fix):
//   - `var f := lambda() => do var y; return lambda() => x + y; end;`
//     `var g := f(); var v := g();` -- LEAKED 4 B in 1 allocation per
//     invocation of Main(); amplified to 2000 B in 500 allocations by the
//     standard loop wrapper.
//   - Same shape but with an IDENTIFIER-bound inner (`var inner := ...;
//     return inner;` instead of a bare literal in `return`) -- LEAKED the
//     SAME 4 B per invocation (the literal-vs-identifier distinction is
//     irrelevant to the missing `escapingCapturersOf` entry).
//   - Same shape but with the inner declared in `var` form (no `var y`
//     block-local; both `x` and `y` are outer-scope variables) -- was
//     already clean (#3804 widening path covers it; the fix preserves the
//     path); pinned as a control.
//
// After the fix: all four rows are leak-clean. The widening to the
// #3805 transparent-tail-non-escaping branch (the variant that fires
// when the outer is `var f := lambda() => do … end; var g := f()` rather
// than `return outer();`) extends the SHARED-CELL envdtor route to the
// tail-returning-via-binding shape: it records `nl` as the SOLE escaping
// capturer of each of `nl`'s captures (so `sharedCellEnvdtorFreeOwner_`
// condition (a) holds), adds those captures to `capturedByNonEscaping`
// (so condition (c) holds), adds `nl` to `definitelyEscapingLambdas_`
// (so condition (b) holds), and skips the conservative
// `nestedWidenedKeys` insertion (so condition (d) holds) — the same
// quadruple the #3804 widening applies, mirrored for the
// transparent-tail twin.

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
// after `var x := 1;`, ending in whatever produces the escaping lambda.
// Run() calls Make() then immediately invokes the escaped result, and
// Main() amplifies via 500 iterations so a per-call leak accumulates to a
// LeakSanitizer-visible total (a one-shot run can pass on both sides of a
// small per-call leak).
std::string program(const std::string& makeBody) {
    return std::string(kHdr) +
        "class C "
        "  public static method Make(): () -> int "
        "    var x := 1; " +
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

// THE FIX: a BLOCK-bodied outer lambda that declares a block-local `y`
// and returns a bare-literal nested lambda capturing BOTH the outer
// `x` AND the block-local `y`; the outer is consumed via `var f :=
// C.Make(); var v := f();` (NOT `return outer();`). Leaked pre-fix
// (2000 B / 500 allocations); clean post-fix.
BEZEL_TEST(closure_nested_lambda_outer_capture_cell_leak_4470,
           block_body_bare_literal_tail_via_let_binding) {
    BEZEL_EXPECT_EQ(
        checkedOut(program(
            "var f := lambda() => do "
            "  var y := 2; "
            "  return lambda() => x + y; "
            "end; "
            "return f(); ")),
        "total=1500\n");
}

// THE FIX (variant): same shape but with the inner bound to a `var`
// local first (`var inner := lambda() => x + y; return inner;`). The
// literal-vs-identifier distinction is irrelevant to the missing
// `escapingCapturersOf` entry, but pinned separately to guard against a
// future regression that only addresses one form.
BEZEL_TEST(closure_nested_lambda_outer_capture_cell_leak_4470,
           block_body_named_local_tail_via_let_binding) {
    BEZEL_EXPECT_EQ(
        checkedOut(program(
            "var f := lambda() => do "
            "  var y := 2; "
            "  var inner := lambda() => x + y; "
            "  return inner; "
            "end; "
            "return f(); ")),
        "total=1500\n");
}

// CONTROL: same shape but the inner captures ONLY the outer-scope `x`
// (no block-local `y`). Already clean on HEAD before this fix (the
// #3804 transparent-forwarder widening handles the `return inner;` form
// because `inner` is reachable through `outer`'s forward-target map);
// pinned as a regression guard so a future widening regression here is
// caught by the same file rather than only by the existing
// closure_grandparent_block_body_escape_3704 sibling.
BEZEL_TEST(closure_nested_lambda_outer_capture_cell_leak_4470,
           block_body_only_outer_capture_regression_guard) {
    BEZEL_EXPECT_EQ(
        checkedOut(program(
            "var f := lambda() => do "
            "  var inner := lambda() => x; "
            "  return inner; "
            "end; "
            "return f(); ")),
        "total=500\n");
}
