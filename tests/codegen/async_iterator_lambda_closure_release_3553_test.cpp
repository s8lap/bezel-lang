// Codegen — issue #3553 (spec §27.5 "Closures", §28.9 "Runtime-managed types",
// §30.7 "Async iterators", v0.283): a `var` LAMBDA local in an ASYNC ITERATOR
// body never released its closure.
//
//     public static async method Gen(n: int): IAsyncEnumerable<int>
//       var f := lambda(x: int) => x + 1;
//       yield return f(n);
//       yield return f(n + 1);
//     end Gen;
//
//     $ bezelc --sanitize=address -o p p.bzl && ./p     # 200 enumerations
//     acc=40400
//     Direct leak of 8000 byte(s) in 200 object(s) allocated from:
//         #1 bz$asynciter$Gen$Resume
//         #2 bz$asynciter$Gen$MoveNextAsync
//
// The SYNC iterator twin and the plain-async-method twin of the same body are
// both clean, and so is the async iterator with the lambda removed — which is
// what localises the defect to the async-iterator resume path.
//
// ── WHAT WAS WRONG. Not a pass that mis-answered: the pass never ran.
// `emitAsyncIteratorMethod` builds its resume body with the usual
// "re-establish this analysis for the resume fn" preamble, and had accumulated
// four members of that family over time — `unwindIneligibleFn_` (#1896/#1897),
// `collectUnwindIneligible`, `collectStringReleaseEligible` /
// `stringReleaseEligibleFn_` (#2817), `smCellFrameFn_` (#3466). The closure
// half was missing, so `closureReleaseEligibleFn_` still held whatever
// `llvm::Function*` the PREVIOUSLY emitted method left behind,
// `currentFn_ == closureReleaseEligibleFn_` was false for this entire body, and
// the release never registered in the resume's `disposeStack_`.
//
// Every sibling emitter has the two lines and says why. From
// `emitAsyncMethod`: "The async routing in emitFunctionBody returns BEFORE that
// pass's collectClosureReleaseEligible / eligible-fn setup runs, so without
// this the closure cell (+ its env block) leaks." The sync iterator's
// `emitIteratorMethod` keys the identical pair to `moveNextFn`. This commit
// keys it to `resumeFn`.
//
// ── DIRECTION. This ARMS a release, so the risk is an over-release, not a
// leak. Rows 1-4 are `runAotChecked` (generated-code ASan with LeakSanitizer
// on), so a double free or use-after-free fails the row before the value is
// compared. Row 5 is the deliberate over-release probe: the same lambda handed
// to ANOTHER function, where `collectClosureReleaseEligible` — the existing
// authority this commit feeds, not a new predicate — must DECLINE eligibility.
// It is a `plainOut` row because that shape still leaks (unchanged by this
// commit, both sides 10400 B / 600); its oracle is the printed value, which a
// premature release would corrupt or crash.
//
// ── RESIDUE, recorded so a later reader does not mistake it for this fix. A
// lambda that CAPTURES a local still leaks its 4-byte capture cell (800 B over
// 200 activations) after this commit, down from 10400 B / 600 allocations. That
// belongs to the capture-cell reclaim family (#3498 / #3500 / #3518 / #3532),
// not to the closure cell this commit releases, and it is why rows 1-4 use a
// NON-capturing lambda: their LSan oracle has to be able to reach zero.
// per §27.5, §28.9, §30.7 (issue #3553)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan with LeakSanitizer
// on, so a leak, a double free or a use-after-free fails the row before the
// printed value is compared.
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
// Plain runAot for the one row whose shape still leaks by design (see RESIDUE).
std::string plainOut(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── Row 1, the issue's repro: a non-capturing `var` lambda local in an
// `IAsyncEnumerable<int>` body, 200 enumerations. Pre-fix `Direct leak of 8000
// byte(s) in 200 object(s)` from `bz$asynciter$Gen$Resume`; the printed total is
// unaffected either way, so LSan is the ONLY oracle that sees this defect and
// this row must not be weakened to a plain value check.
// acc = sum over j in 0..<200 of ((j+1) + (j+2)) = 2*sum(j) + 600 = 40400.
// per §27.5, §28.9, §30.7 (issue #3553)
BEZEL_TEST(async_iterator_lambda_closure_release_3553,
           lambda_local_in_an_async_iterator_releases_its_closure_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "import System.Threading.Tasks; "
        "class C "
        "  public static async method Gen(n: int): IAsyncEnumerable<int> "
        "    var f := lambda(x: int) => x + 1; "
        "    yield return f(n); "
        "    yield return f(n + 1); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      await for var v in C.Gen(j) do acc := acc + v; end; "
        "    end; "
        "    Console.WriteLine(\"acc=\" + acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "acc=40400\n");
}

// ── Row 2, the NO-LAMBDA control: the identical iterator with the lambda
// replaced by the arithmetic it performed. Clean before and after — it is what
// says the leak was the closure and not the async-iterator frame itself.
// per §30.7 (issue #3553)
BEZEL_TEST(async_iterator_lambda_closure_release_3553,
           async_iterator_without_a_lambda_is_unchanged_spec_30_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "import System.Threading.Tasks; "
        "class C "
        "  public static async method Gen(n: int): IAsyncEnumerable<int> "
        "    yield return n + 1; "
        "    yield return n + 2; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      await for var v in C.Gen(j) do acc := acc + v; end; "
        "    end; "
        "    Console.WriteLine(\"acc=\" + acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "acc=40400\n");
}

// ── Row 3, the SYNC-ITERATOR twin: byte-identical body through
// `emitIteratorMethod`, which has had the two lines all along. Clean on both
// sides; this row is the one that says the remedy is parity with an existing
// site rather than a new rule.
// per §27.5, §28.9 (issue #3553)
BEZEL_TEST(async_iterator_lambda_closure_release_3553,
           sync_iterator_twin_of_the_same_lambda_body_stays_clean_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class C "
        "  public static method Gen(n: int): IEnumerable<int> "
        "    var f := lambda(x: int) => x + 1; "
        "    yield return f(n); "
        "    yield return f(n + 1); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      for var v in C.Gen(j) do acc := acc + v; end; "
        "    end; "
        "    Console.WriteLine(\"acc=\" + acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "acc=40400\n");
}

// ── Row 4, the PLAIN-ASYNC-METHOD twin: the other emitter that already keys
// the pair to its own `resumeFn`. Clean on both sides — this commit must not
// have disturbed it, since it edits the same file.
// per §27.5, §30.7 (issue #3553)
BEZEL_TEST(async_iterator_lambda_closure_release_3553,
           plain_async_method_twin_of_the_same_lambda_body_stays_clean_spec_30_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Threading.Tasks; "
        "class C "
        "  public static async method Work(n: int): Task<int> "
        "    var f := lambda(x: int) => x + 1; "
        "    await Task.Delay(0); "
        "    return f(n) + f(n + 1); "
        "  end Work; "
        "  public static async method Main(): Task<int> "
        "    var acc := 0; "
        "    for var j in 0..<200 do acc := acc + await C.Work(j); end; "
        "    Console.WriteLine(\"acc=\" + acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "acc=40400\n");
}

// ── Row 5, the OVER-RELEASE probe: the same lambda is handed to ANOTHER
// function, so its closure escapes the body and
// `collectClosureReleaseEligible` — the existing authority this commit feeds —
// must DECLINE to make it eligible. If the newly-armed release fired here it
// would free a closure `H.Keep` is still using, which shows up as a wrong
// `k=` total or an ASan use-after-free. It is a `plainOut` row on purpose: the
// escaping shape still leaks (10400 B / 600 allocations, IDENTICAL before and
// after this commit — the residue is not this commit's to fix, and an LSan row
// here would enshrine it).
// acc = 40400 as above; k = sum over j in 0..<200 of (1 + j) = 20100.
// per §27.5, §28.9 (issue #3553)
BEZEL_TEST(async_iterator_lambda_closure_release_3553,
           escaping_closure_in_an_async_iterator_is_not_released_early_spec_27_5) {
    BEZEL_EXPECT_EQ(plainOut(
        "module M; import System; import System.Collections.Generic; "
        "import System.Threading.Tasks; "
        "class H "
        "  public static k: int := 0; "
        "  public static method Keep(f: (int) -> int): int "
        "    H.k := H.k + f(1); "
        "    return 0; "
        "  end Keep; "
        "end H; "
        "class C "
        "  public static async method Gen(n: int): IAsyncEnumerable<int> "
        "    var f := lambda(x: int) => x + n; "
        "    var d := H.Keep(f); "
        "    yield return f(1) + d; "
        "    yield return f(2); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      await for var v in C.Gen(j) do acc := acc + v; end; "
        "    end; "
        "    Console.WriteLine(\"acc=\" + acc.ToString() + \" k=\" + H.k.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "acc=40400 k=20100\n");
}
