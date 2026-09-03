// Codegen: §27.5 / §28.9 (v0.286) — a capturing closure passed as an ARGUMENT
// is a TEMPORARY whose envdtor fires at end of the call statement, before any
// later read of a captured local — heap-use-after-free (issue #3778).
//
// WHAT IT ACTUALLY IS. #3778 was filed as the same-depth sibling of #3752
// that the #3752 fix could not reach: the §27.5 capture model transfers the
// captured local's single +1 into the FIRST capturing env's envdtor, and
// #3752 moved that release to the captured local's OWN scope under the gate
// `declScopeDepth_[decl] < disposeStack_.size() && cellFreeEligible_`. A
// closure passed as an argument is a TEMPORARY — released by the callee at
// end of the call statement — at the SAME dispose depth as the captured
// local, so `declaredShallower` is false and the gate declines. Its envdtor
// still performs the "deferred scope-end release" and the outer local is
// then dangling for the rest of its scope. Measured on 3729220 (the
// post-#3752 tree):
//
//     var f := lambda(): int => 11;
//     var total := Apply(lambda(): int => f());
//     return total + f();     <- reads freed 40-byte closure cell
//
//   ASan: `heap-use-after-free ... in bz$M.C$Take`; `freed by ... in
//   bz$lam$1$envdtor`.
//
// THE FIX widens the #3752 gate in two coordinated pieces:
//
//   1. PRE-SCAN: `isCallBoundedClosureArgPosition` accepts non-function-
//      typed returns (it used to require NO declared return type at all,
//      deliberately over-conservative for #3498). The callee in the issue's
//      repro returns `int`, which cannot carry a function-typed value out,
//      and is otherwise not storing the closure in any long-lived location,
//      so the captured local lands in `cellFreeEligible_` exactly like an
//      IIFE. The async / yield-body / named-arg / `owned`-arg / `scoped`-
//      arg exclusions stay in place — those are real escape paths.
//
//   2. GATE: `lowerLambda`'s first-capture content-release gate drops
//      `declaredShallower` in favor of `(declaredShallower || inArgumentLambda_)`,
//      where `inArgumentLambda_` is set by `buildOrderedArgs` around
//      `lowerExpr(*a.value)` for an argument that is a lambda LITERAL.
//      The closure dies at end of statement (its lifetime is bounded by
//      the call), and the deferred release registers at the captured
//      local's own scope — exactly the move #3752 made for a deeper-scope
//      capture, but for a same-depth capture whose lifetime mismatch is a
//      STATEMENT, not a deeper dispose scope.
//
// Both pieces together: the captured local is in `cellFreeEligible_`, the
// gate fires, the envdtor's content release is suppressed, and a single
// `capturedCellContentRelease` disposal runs at the owner's scope end.
// Accounting stays exact — one +1, one release, never both.
//
// Every row below pins a VALUE (a returned int or a printed string), never
// a crash signature: per the two-oracle rule (#2633) a `WIFSIGNALED`
// assertion is oracle-dependent, while a wrong/absent value is not.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

// Each test program is a single class `C` with the helper `Apply` static
// method co-located with `Take`. The callee returns `int`, takes a
// function-typed parameter, stores nothing, and is synchronous. Every
// captured local lives one line away from its `f()` read, so a
// use-after-free inside the test program manifests as a wrong value, not a
// segfault.
const char* kModuleHeader = "module M; ";
const char* kApplyHeader =
    "class C "
    "  public static method Apply(g: () -> int): int return g(); end Apply; ";

} // namespace


// ── §27.5 / §28.9 (v0.286): the defect underneath, pinning the value ─────

BEZEL_TEST(codegen_capturing_lambda_argument_3778, spec_27_5_closure_captured_by_argument_lambda_outlives_it) {
    // The issue's exact repro: a capturing closure passed as an argument is a
    // temporary whose envdtor frees `f` before `f()` is called after. Pre-fix
    // the second `f()` reads a freed 40-byte closure cell — UAF, rc=139.
    // With the fix, `f` lives to its last read and the program returns 22.
    BEZEL_EXPECT_EQ(runAotOut(
        std::string(kModuleHeader) + kApplyHeader +
        "  public static method Take(): int "
        "    var f := lambda(): int => 11; "
        "    var total := Apply(lambda(): int => f()); "
        "    return total + f(); "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"), "22");
}

BEZEL_TEST(codegen_capturing_lambda_argument_3778, spec_27_5_string_captured_by_argument_lambda_outlives_it) {
    // The string half of the same capture model (the envdtor's other content
    // branch). A heap-built string, so no literal/sentinel shortcut hides the
    // freed-memory read — the post-call `s.Length` would see freed chars.
    BEZEL_EXPECT_EQ(runAotOut(
        std::string(kModuleHeader) + kApplyHeader +
        "  public static method Take(): string "
        "    var s := \"he\" + \"llo\"; "
        "    var total := Apply(lambda(): int => s.Length); "
        "    return total.ToString() + \"/\" + s; "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take()); "
        "    return 0; "
        "  end Main; "
        "end C;"), "5/hello");
}


// ── The depth-axis: same-depth, deeper-scope, all pin the value ──────────

BEZEL_TEST(codegen_capturing_lambda_argument_3778, spec_27_5_argument_lambda_in_do_block_outlives_it) {
    // The scope may be a `do … end`: the argument lambda still dies at end
    // of the call statement, which sits inside the captured local's scope.
    // Same shape as #3752's row 3 but the closure is an argument, not bound
    // to a local — that is precisely the surviving hole #3778 named.
    BEZEL_EXPECT_EQ(runAotOut(
        std::string(kModuleHeader) + kApplyHeader +
        "  public static method Take(): int "
        "    var f := lambda(): int => 11; "
        "    var total := 0; "
        "    do "
        "      var n := Apply(lambda(): int => f()); "
        "      total := total + n; "
        "    end; "
        "    return total + f(); "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"), "22");
}

BEZEL_TEST(codegen_capturing_lambda_argument_3778, spec_27_5_argument_lambda_inside_if_branch_outlives_it) {
    // An `if` branch pushes a dispose scope too, so a temporary argument
    // lambda inside one still dies at end of the call statement, before the
    // captured local's scope end. Same fix shape, smaller scope.
    BEZEL_EXPECT_EQ(runAotOut(
        std::string(kModuleHeader) + kApplyHeader +
        "  public static method Take(): int "
        "    var f := lambda(): int => 11; "
        "    var total := 0; "
        "    if true then "
        "      total := Apply(lambda(): int => f()); "
        "    end; "
        "    return total + f(); "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"), "22");
}


// ── Controls: the other shapes the fix MUST NOT move ─────────────────────

BEZEL_TEST(codegen_capturing_lambda_argument_3778, spec_27_5_argument_lambda_capturing_nothing_is_unchanged) {
    // A non-capturing lambda passed as an argument has no captured locals,
    // so the gate has nothing to defer. Pre-fix and post-fix both run the
    // call and free the closure; nothing about the envdtor's behaviour
    // moves. The control that says the fix did not over-reach.
    BEZEL_EXPECT_EQ(runAotOut(
        std::string(kModuleHeader) + kApplyHeader +
        "  public static method Take(): int "
        "    return Apply(lambda(): int => 11 + 22); "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"), "33");
}

BEZEL_TEST(codegen_capturing_lambda_argument_3778, spec_27_5_locally_bound_capture_is_unchanged) {
    // A capturing closure BOUND to a local (`var g := lambda(): int => f();`)
    // is the shape #3752 already handled, and the new widening widens only
    // `inArgumentLambda_` — the local-bound sibling is unchanged in its
    // lifetime answer and its release point stays at the envdtor / deferred
    // site the existing gate chose. Returns 22 on both sides of the fix.
    BEZEL_EXPECT_EQ(runAotOut(
        std::string(kModuleHeader) +
        "class C "
        "  public static method Take(): int "
        "    var f := lambda(): int => 11; "
        "    var g := lambda(): int => f(); "
        "    return g() + f(); "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"), "22");
}


// ── §28.9 accounting is EXACT, not a leak trade ──────────────────────────
//    runAotChecked = generated-code ASan + LeakSanitizer. Loop-amplified to
//    500 rounds: a one-shot row cannot see a leak (the last round's
//    allocation is still reachable at exit), and a one-shot UAF can land in
//    mapped memory.

BEZEL_TEST(codegen_capturing_lambda_argument_3778, spec_28_9_closure_argument_lambda_x500_is_memory_clean) {
    auto r = runAotChecked(
        std::string(kModuleHeader) + kApplyHeader +
        "  public static method Round(): int "
        "    var f := lambda(): int => 11; "
        "    var total := Apply(lambda(): int => f()); "
        "    return total + f(); "
        "  end Round; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 500 do "
        "      acc := acc + Round(); "
        "      i := i + 1; "
        "    end; "
        "    if acc <> 11000 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(codegen_capturing_lambda_argument_3778, spec_28_9_string_argument_lambda_x500_is_memory_clean) {
    auto r = runAotChecked(
        std::string(kModuleHeader) + kApplyHeader +
        "  public static method Round(): int "
        "    var s := \"he\" + \"llo\"; "
        "    var total := Apply(lambda(): int => s.Length); "
        "    return total + s.Length; "
        "  end Round; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 500 do "
        "      acc := acc + Round(); "
        "      i := i + 1; "
        "    end; "
        "    if acc <> 5000 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}


// ── A sibling-bound control: the local the fix's gate widens MUST still
//    work when there is no captured local to defer to. The temporary
//    closure is released at end of statement regardless, so this is the
//    regression guard for "the fix did not introduce a permanent closure
//    leak on the temporary-argument path".

BEZEL_TEST(codegen_capturing_lambda_argument_3778, spec_28_9_temporary_argument_lambda_releases_at_end_of_statement) {
    auto r = runAotChecked(
        std::string(kModuleHeader) + kApplyHeader +
        "  public static method Round(): int "
        "    var f := lambda(): int => 11; "
        "    return Apply(lambda(): int => f()) + f(); "
        "  end Round; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 500 do "
        "      acc := acc + Round(); "
        "      i := i + 1; "
        "    end; "
        "    if acc <> 11000 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
