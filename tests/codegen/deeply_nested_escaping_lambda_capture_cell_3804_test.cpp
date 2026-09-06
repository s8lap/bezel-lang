// Codegen — deeply nested ESCAPING lambdas (both expression- and
// block-bodied outer forms) leak the shared capture cell AND the escaped
// closure itself (§27.5 / §28.9, #3804).
//
// Spec §27.5: "A lambda may reference variables from its enclosing scope.
// Captured variables are heap-allocated in a closure object." Spec §28.9:
// "Function-typed values ... regardless of whether they capture ... Runtime
// management governs the *function value itself* — the heap-allocated
// closure object that backs it".
//
// ⚠ An earlier draft of this comment continued the §28.9 quote with "no
// runtime lifetime mechanism exists for user-defined classes at all ...
// [e]very [heap object] the user can write ... is freed by compiler-
// inserted code at a statically-known point", bracket-substituting
// "class" -> "[heap object]" and calling it "the same deterministic-
// reclaim guarantee this fix restores". That was wrong and is removed:
// the quoted sentence is specifically about user-defined CLASSES, which
// §28.9 elsewhere expressly contrasts with function-typed values, whose
// "lifecycle ... is not part of this specification". The substitution
// imported a class-specific guarantee onto a category the same section
// leaves unspecified. What this fix actually restores is narrower and
// needs no spec guarantee to state: the closure header, its env, and
// every capture cell are released when the closure's refcount reaches
// zero, instead of leaking unconditionally.
//
// #3804's own probes (both bodies of `outer` below): a `var outer :=
// lambda() => ...;` whose call produces exactly one nested lambda (`inner`,
// bound at a `var inner :=` position inside a BLOCK body, or the bare
// expression body itself) that genuinely ESCAPES `outer` (returned out of
// it) — and `outer` itself is called directly, with no intermediate
// binding, as `return outer();`.
//
// TWO independent, compounding root causes, BOTH required for a complete
// fix (found via ASan/LSan disassembly of `bz$M.C$Make`/`bz$M.C$Run` on
// this exact probe, not by re-deriving from the issue's stale R13-era
// analysis, which predates fix #3766/#3805/#3823 and no longer describes
// current behavior):
//
//   (1) `lowerReturnStmt`'s closure-RETAIN guard (LlvmBackend_Stmts.cpp)
//       checked `isPlusOneClosureProducer || isUserMethodClosureReturnCall
//       || returnedClosureLocalOwnsPlusOne` but NOT `isLambdaClosureReturnCall`
//       (fix #2714 — a CallExpr whose callee is a lambda-typed LOCAL/
//       PARAMETER). So `return outer();` minted a SECOND +1 on a reference
//       `outer`'s own body already transferred, inflating the returned
//       closure's refcount by one. `bz_closure_release` (called correctly,
//       once, by `Run`'s own scope-end disposal of `f`) then only brought
//       the count from 2 to 1 — never zero — so `inner`'s envdtor NEVER
//       RAN: the closure header (40 B), its env (8 B/capture), and every
//       capture cell it held leaked, unconditionally, regardless of (2).
//
//   (2) Even with (1) fixed, `collectCellFreeEligible`'s §3426
//       `sharedCellEnvdtorFreeOwner_` map — freshly recomputed, from
//       scratch, at EVERY nested lambda scope's own `lowerLambda` (fix
//       #3757's own re-pin mechanism) — has no way to see that `outer`
//       (the ANCESTOR scope analyzing `x`) already proved `inner` is the
//       sole safe envdtor-owner for `x`'s cell: `outer`'s OWN re-scan of
//       ITS OWN body cannot rediscover "some non-escaping lambda ALSO
//       captures x" (§3426 condition (c)), because `outer` does not
//       capture itself. The ancestor's answer, computed once correctly
//       over the WHOLE function body, was silently discarded at the exact
//       point `inner`'s own envdtor gets generated.
//
// Fix: (1) adds `!isLambdaClosureReturnCall(rs->value.get())` to BOTH
// closure-retain guards in `lowerReturnStmt` (the method-return arm and its
// lambda-body-return twin). (2) adds `collectTransparentForwardTargets` — a
// single shared authority (replacing fix #3805's own inline, bare-
// expression-body-only `forwardTarget3805` loop) that recognizes EITHER a
// bare-expression-body OR a STRAIGHT-LINE block body (declining outright the
// instant any control-flow statement appears) as transparently forwarding a
// nested lambda — consumed by `collectDefinitelyEscapingLambdas` (marks the
// forwarded target definitely-escaping when reached via `return outer();`,
// gated on `outer` being called AT MOST ONCE — soundness row below) and by
// `collectLambdaCaptures`'s §1135 widening (attributes the forwarded
// target as a DIRECT escaping capturer instead of the anonymous, owner-less
// `nestedWidenedKeys` bucket condition (d) always excludes). Exprs2.cpp's
// nested-lambda-scope re-scan additionally MERGES the ancestor's already-
// computed `sharedCellEnvdtorFreeOwner_` entries forward (keyed by AST node,
// `emplace`-only so the fresh local recompute always wins for any key it
// DOES know about) so `inner`'s own envdtor generation can see the
// grandparent's decision.
//
// LEAK AMPLIFICATION (§lsan-one-shot-leak-tests-pass-vacuously): a one-shot
// run cannot distinguish "freed" from "still stack-reachable at exit" — both
// probes iterate 500 times under ASan/LSan, matching the amount originally
// measured (26000 B / 1500 allocations at x500, pre-fix, for BOTH bodies
// identically).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// runChecked() — AOT under AddressSanitizer/LSan. A leak (or a UAF) makes
// ASan/LSan abort with a non-zero exit status; a clean run reports 0. Plain
// `runAot` cannot see either (a plain allocator + no fault interception), so
// it is never used in this file.
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

std::string amplifiedProgram(const std::string& makeBody) {
    return
        "module M; import System; "
        "class C "
        "  public static method Make(): () -> int " + makeBody +
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
        "    Console.WriteLine(total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

}  // namespace

// ============================================================================
// Row 1 (LEAK-AMPLIFIED x500, BLOCK BODY): issue #3804's own block-bodied
// probe. `outer`'s block body declares `inner` at a `var inner :=` position
// and returns it — `inner` genuinely escapes `outer` (and, via `outer()`'s
// direct use as Make's return value with no intermediate binding, escapes
// `Make` itself). Pre-fix: 26000 B / 1500 allocations leaked (measured on
// this lane's own HEAD before either fix landed). Post-fix: clean.
// ============================================================================
BEZEL_TEST(codegen_deeply_nested_escaping_lambda_3804,
           block_body_outer_leak_amplified_500x) {
    std::string src = amplifiedProgram(
        "    var x := 3; "
        "    var outer := lambda() => do "
        "      var inner := lambda() => x; "
        "      return inner; "
        "    end; "
        "    return outer(); ");
    BEZEL_EXPECT_EQ(runChecked(src), 0);
}

// ============================================================================
// Row 2 (LEAK-AMPLIFIED x500, EXPRESSION BODY): the same shape with `outer`'s
// entire body being the bare nested lambda literal (no `var inner :=`
// binding at all — `collectTransparentForwardTargets`'s bare-expression-body
// case, fix #3805's original shape, but here the forwarded target genuinely
// ESCAPES rather than being consumed non-escapingly within the same body).
// Pre-fix: identical 26000 B / 1500 allocations (measured — BOTH body forms
// leak the same amount, refuting the issue's original "only block-body
// leaks" premise, matching R13's b92 finding). Post-fix: clean.
// ============================================================================
BEZEL_TEST(codegen_deeply_nested_escaping_lambda_3804,
           expression_body_outer_leak_amplified_500x) {
    std::string src = amplifiedProgram(
        "    var x := 3; "
        "    var outer := lambda() => lambda() => x; "
        "    return outer(); ");
    BEZEL_EXPECT_EQ(runChecked(src), 0);
}

// ============================================================================
// Row 3 (SOUNDNESS, guard-sensitivity): `outer` is called TWICE, producing
// TWO independent `inner` closures that share the SAME already-boxed cell
// for `x`. `inner1` is used and discarded locally (non-escaping); `inner2`
// ESCAPES by being returned from `Run` and invoked from `Main`, STRICTLY
// AFTER `Run` has returned — the same "read after the producing scope is
// gone" shape #3805's own soundness rows use, so a wrongly-freed cell would
// surface as a heap-use-after-free, not silently pass.
//
// `calledAtMostOnce` (computed once, in collectTransparentForwardTargets,
// by the identical ClosureEscapeWalker technique fix #3805 trusts for `M :=
// L()`, applied here to `outer` itself) must read FALSE for this program
// (`outer` has two call sites) — both this fix's new
// collectDefinitelyEscapingLambdas admission and its collectLambdaCaptures
// direct-attribution branch must DECLINE, falling back to the pre-existing
// conservative `nestedWidenedKeys` leak (never a false "sole owner" claim
// naming one AST node as though only one runtime instance existed). Asserts
// the program runs cleanly under ASan (no UAF) with the expected value —
// the pre-existing conservative leak (x's cell, never disposed by anyone)
// is the ACCEPTED trade-off this row is pinning, not a defect.
// ============================================================================
BEZEL_TEST(codegen_deeply_nested_escaping_lambda_3804,
           outer_called_twice_one_escapes_no_uaf) {
    std::string src =
        "module M; "
        "class C "
        "  public static method Run(): () -> int "
        "    var x := 11; "
        "    var outer := lambda() => do "
        "      var inner := lambda() => x; "
        "      return inner; "
        "    end; "
        "    var inner1 := outer(); "
        "    var inner2 := outer(); "
        "    var r: int := inner1(); "
        "    return inner2; "
        "  end Run; "
        "  public static method Main(): int "
        "    var escaped := C.Run(); "
        "    var n: int := escaped(); "
        "    return n; "
        "  end Main; "
        "end C;";
    auto r = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(r.ran);       // ASan would have aborted on a UAF
    BEZEL_EXPECT_EQ(r.exitCode, 11);  // both closures still read x == 11
}

// ============================================================================
// Row 4 (plain venue, SINGLE iteration, no amplification loop): measured to
// ALSO fail pre-fix — `runAotChecked`'s `r.ran` reads false (ASan aborts
// rather than exiting normally) even at one call, not only under the x500
// amplification rows 1/2 use. Kept as its own row anyway: it isolates the
// bug from the amplification loop entirely, so a future regression here
// localizes to "the plain single-call shape is broken again" rather than
// "something about looping 500 times changed".
// ============================================================================
BEZEL_TEST(codegen_deeply_nested_escaping_lambda_3804,
           block_body_plain_venue_single_iteration) {
    std::string src =
        "module M; "
        "class C "
        "  public static method Make(): () -> int "
        "    var x := 42; "
        "    var outer := lambda() => do "
        "      var inner := lambda() => x; "
        "      return inner; "
        "    end; "
        "    return outer(); "
        "  end Make; "
        "  public static method Main(): int "
        "    var f := C.Make(); "
        "    return f(); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
