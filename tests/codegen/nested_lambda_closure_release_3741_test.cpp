// Codegen — issue #3741 (spec §27.5 "Closures", §28.3 "Local variables,
// ownership, and borrows", §28.9 "Runtime-managed types", v0.286): a
// DOUBLY-nested lambda local — a `var g := lambda...` whose OWN body
// declares and invokes a second lambda local `h` — never released `h`'s
// 40-byte closure. The issue found this two levels deep inside an ASYNC
// LAMBDA body (a single level of nesting there is clean); this fix's own
// measurement (agent_check --probe --asan against r12-cycle-1 2e04978,
// before this commit's src/ change) shows the IDENTICAL `8000 B / 200`
// `lam$0`/`lam$1` signature for the SAME two-level nesting inside a PLAIN
// SYNC METHOD with no async machinery anywhere — so the issue's own
// "isolates the async-lambda axis" framing does not hold: the gap is not an
// async-lambda-specific eligibility decision, it is `lowerLambda`'s SYNC
// branch (LlvmBackend_Exprs2.cpp) never establishing closure-release
// eligibility for its OWN body at all, in ANY venue.
//
// ROOT CAUSE. `collectClosureReleaseEligible(body)` scans only ITS OWN
// `body` statement list (control-flow recurses; a NESTED LambdaExpr's own
// statements do not — that nested body earns its own establishment when its
// own venue lowers it). Every TOP-LEVEL venue (a plain method, a destructor,
// a sync iterator's MoveNext, an async method/iterator/lambda's resume fn)
// runs this establishment for itself, keyed to its own `llvm::Function*`.
// A SYNC LAMBDA nested inside any of those IS its own venue (its own
// `llvm::Function* fn`) but `lowerLambda`'s sync branch never ran the
// establishment for it: `closureReleaseEligible_` / `closureReleaseEligibleFn_`
// stayed exactly what the ENCLOSING body last set them to — never `fn` — so
// the consumer's gate (`currentFn_ == closureReleaseEligibleFn_`,
// LlvmBackend_Setup.cpp lowerLocalDecl) was unconditionally false for the
// whole time `currentFn_ == fn`, and a lambda-local declared directly in
// THIS body (any depth) never registered its scope-end `bz_closure_release`.
// A single level of nesting was invisible to this gap because the OUTER
// venue's OWN establishment (scanning the outer body's own statement list)
// already finds and correctly registers a lambda-local declared directly in
// ITS body, before ever calling into the sync branch for that lambda's OWN
// (now correctly analyzed) contents.
//
// FIX. Mirror the establishment every other venue already runs — for the
// sync lambda's OWN body, keyed to its OWN `fn` — with the same save/restore
// discipline (LANE_RULES §8) the async branch already uses around
// `emitAsyncLambda`'s identical establishment, so a later lambda-local in the
// ENCLOSING body is unaffected.
//
// DANGER DIRECTION (this is an ARMING fix, so the risk is DOUBLE FREE, not a
// leak). `ClosureEscapeWalker` — the walk `collectClosureReleaseEligible`
// runs — depends only on the static `SemaContext` and the `body` handed to
// it, so recomputing it here cannot retroactively mis-admit anything already
// registered against a DIFFERENT `currentFn_`. Row 6 below is the capture
// control: a closure captured BY a more-deeply-nested lambda is excluded by
// the SAME `inLambda` escape rule that already protects every other venue —
// runAotChecked would crash on a double free before printing, so a `plainOut`
// row asserting the printed VALUE is itself proof nothing was over-released
// (identical precedent to async_iterator_lambda_closure_release_3553_test.cpp
// Row 5). That row also has a SEPARATE, PRE-EXISTING residual leak (h's own
// capture CELL, ~8 B/activation — verified present, in fact larger, on the
// parent commit 2e04978 with this fix's src/ reverted: 19200 B / 800 there
// vs 1600 B / 200 after this fix) that belongs to the capture-cell-reclaim
// family (#3498/#3500/#3518/#3532), not to the closure release this commit
// fixes — filed separately (see the commit body) rather than folded in here.
//
// §6b enumeration (depth x venue x escape shape), each verified under
// runAotChecked (generated-code ASan + LeakSanitizer) at 200 activations:
//   Row 1: depth 2, ASYNC LAMBDA venue (the issue's own repro)
//   Row 2: depth 2, plain SYNC METHOD venue (no async anywhere)
//   Row 3: depth 3, ASYNC LAMBDA venue (three lambda locals deep)
//   Row 4: depth 2, ASYNC METHOD venue (not lambda)
//   Row 5: depth 2, SYNC ITERATOR (MoveNext) venue
//   Row 6: depth 2, CAPTURE control (must NOT double-release) — plainOut
// per §27.5, §28.3, §28.9 (issue #3741)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan with LeakSanitizer
// on, so a leak, a double free, or a use-after-free fails the row before the
// printed value is compared.
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
// Plain runAot for Row 6, whose shape still carries a separate, pre-existing
// (not introduced by this fix) capture-cell leak — see the header comment.
std::string plainOut(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── Row 1, the issue's own repro: `async lambda() => do var g := lambda()
// => do var h := lambda() => 5; ... end; ... end;` — a sync lambda local `g`
// directly in an async lambda body, whose OWN body declares a second sync
// lambda local `h`. Pre-fix: `Direct leak of 8000 byte(s) in 200 object(s)`
// from `lam$1`. 200 activations. per §27.5, §28.9, §30.6 (issue #3741)
BEZEL_TEST(nested_lambda_closure_release_3741,
           doubly_nested_sync_lambda_in_async_lambda_releases_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Threading; "
        "import System.Threading.Tasks; "
        "class C "
        "  public static async method RunSyncSyncSync(): Task<int> "
        "    var outer := async lambda() => do "
        "      var g := lambda() => do "
        "        var h := lambda() => 5; "
        "        var x := h(); "
        "      end; "
        "      g(); "
        "      await Task.Delay(1); "
        "    end; "
        "    await outer(); "
        "    return 1; "
        "  end RunSyncSyncSync; "
        "  public static async method Main(): Task<int> "
        "    var totalG := 0; "
        "    for var j in 0..<200 do "
        "      totalG := totalG + await C.RunSyncSyncSync(); "
        "    end; "
        "    Console.WriteLine(\"G=\" + totalG.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "G=200\n");
}

// ── Row 2, the SYNC-METHOD-only twin — no async lambda, no async method, no
// iterator: the SAME two-level nesting inside a plain synchronous method.
// Pre-fix: the IDENTICAL `8000 byte(s) in 200 object(s)` signature (measured
// from `lam$0`, the top-level `main` frame instead of an async resume fn),
// which is what refutes the issue's own "isolates the async-lambda axis"
// framing (see header comment) — the defect is `lowerLambda`'s sync branch
// itself, not an async-lambda-specific decision. 200 activations.
// per §27.5, §28.3, §28.9 (issue #3741)
BEZEL_TEST(nested_lambda_closure_release_3741,
           doubly_nested_sync_lambda_in_plain_sync_method_releases_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method RunSyncSyncSync(): int "
        "    var g := lambda() => do "
        "      var h := lambda() => 5; "
        "      var x := h(); "
        "      return x; "
        "    end; "
        "    return g(); "
        "  end RunSyncSyncSync; "
        "  public static method Main(): int "
        "    var totalG := 0; "
        "    for var j in 0..<200 do totalG := totalG + C.RunSyncSyncSync(); end; "
        "    Console.WriteLine(\"G=\" + totalG.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "G=1000\n");
}

// ── Row 3, DEPTH 3: three sync lambda locals deep inside an async lambda
// body (`g` -> `h` -> `k`), proving the fix is not a one-level-deeper patch
// but a general re-establishment that recurses correctly through
// `lowerLambda` at every level (each level's own sync-branch invocation
// establishes eligibility for its OWN body). 200 activations.
// per §27.5, §28.9, §30.6 (issue #3741)
BEZEL_TEST(nested_lambda_closure_release_3741,
           triply_nested_sync_lambda_in_async_lambda_releases_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Threading; "
        "import System.Threading.Tasks; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var outer := async lambda() => do "
        "      var g := lambda() => do "
        "        var h := lambda() => do "
        "          var k := lambda() => 7; "
        "          var y := k(); "
        "          return y; "
        "        end; "
        "        var x := h(); "
        "        return x; "
        "      end; "
        "      g(); "
        "      await Task.Delay(1); "
        "    end; "
        "    await outer(); "
        "    return 1; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Run(); end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=200\n");
}

// ── Row 4, the ASYNC METHOD venue (not lambda): a doubly-nested sync lambda
// local declared directly in an `async method`'s own body (a venue whose
// top-level establishment — `emitAsyncMethod`'s own
// `collectClosureReleaseEligible` — was never in question; only the SECOND
// level, inside `g`'s own body, was). `x` (== 5) confirms the value survives
// unperturbed. 200 activations. per §27.5, §28.9, §30.1 (issue #3741)
BEZEL_TEST(nested_lambda_closure_release_3741,
           doubly_nested_sync_lambda_in_async_method_releases_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Threading; "
        "import System.Threading.Tasks; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var g := lambda() => do "
        "      var h := lambda() => 5; "
        "      var x := h(); "
        "      return x; "
        "    end; "
        "    await Task.Delay(1); "
        "    return g(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Run(); end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=1000\n");
}

// ── Row 5, the SYNC ITERATOR (MoveNext) venue: a doubly-nested sync lambda
// local declared directly in an `IEnumerable<int>` iterator body. total =
// sum over j in 0..<200 of (9 + j) = 200*9 + 19900 = 21700, confirming both
// the value and the absence of a leak/double-free at 200 activations.
// per §27.5, §28.9, §29.1 (issue #3741)
BEZEL_TEST(nested_lambda_closure_release_3741,
           doubly_nested_sync_lambda_in_sync_iterator_releases_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class C "
        "  public static method Gen(n: int): IEnumerable<int> "
        "    var g := lambda() => do "
        "      var h := lambda() => 9; "
        "      var x := h(); "
        "      return x; "
        "    end; "
        "    yield return g() + n; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      for var v in C.Gen(j) do total := total + v; end; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=21700\n");
}

// ── Row 6, the CAPTURE control (danger direction: must NOT double-release).
// `h` is captured by a THIRD, still-more-nested lambda `k` (`k`'s body reads
// `h` — a call `h()`), so `h`'s own closure must NOT be released at `g`'s
// scope end (§6b: capture ⇒ no release; the released reference belongs to
// whatever eventually disposes `k`'s own captured-cell chain). A double free
// here would crash `runAotChecked`/`runAot` before the value is ever
// printed, or corrupt the string built from `total`; a premature release
// that merely leaves a dangling capture would make `k()` (called from `z :=
// k();`) read freed memory under ASan. total = 200 * 11 = 2200. Plain
// `runAot` (not leak-checked): this shape has a separate, PRE-EXISTING
// capture-cell leak this commit does not fix (verified LARGER — 19200 B /
// 800 — on the parent commit with this fix's src/ change reverted, so it
// predates this fix; 1600 B / 200 remains after it, the pre-existing open capture-cell family #3498 / #3500).
// per §27.5, §28.9 (issue #3741)
BEZEL_TEST(nested_lambda_closure_release_3741,
           captured_nested_lambda_is_not_double_released_spec_27_5) {
    BEZEL_EXPECT_EQ(plainOut(
        "module M; import System; import System.Threading; "
        "import System.Threading.Tasks; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var outer := async lambda() => do "
        "      var g := lambda() => do "
        "        var h := lambda() => 11; "
        "        var k := lambda() => h(); "
        "        var z := k(); "
        "        return z; "
        "      end; "
        "      var r := g(); "
        "      await Task.Delay(1); "
        "      return r; "
        "    end; "
        "    var v := await outer(); "
        "    return v; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Run(); end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=2200\n");
}
