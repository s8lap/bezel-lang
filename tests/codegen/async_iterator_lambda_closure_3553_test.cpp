// Codegen — issue #3553 (spec §27.5 "Closures", §28.9 "Runtime-managed types",
// §30.7 "Async iterators", §31.2, v0.286): a lambda local declared in an ASYNC
// ITERATOR body leaked its closure. This file pins the CAPTURING half of that
// issue, which the two fixes below closed in sequence and which nothing pinned.
//
// ── WHY A SECOND FILE. `async_iterator_lambda_closure_release_3553_test.cpp`
// was written alongside `narrow #3553` (fcb228e), which armed
// `collectClosureReleaseEligible` / `closureReleaseEligibleFn_` for
// `bz$asynciter$*$Resume` (src/codegen/LlvmBackend_Async.cpp, the
// `emitAsyncIteratorMethod` preamble at ~L3256) and so released the closure
// cell and its env block. That file says so in its own RESIDUE note: its rows
// use a NON-capturing lambda on purpose, because a CAPTURING one still leaked
// its capture cell at that commit and an LSan row could not have reached zero.
//
// The residue was then closed by `fix #3507` (eea0ebe), which re-established
// the capture-cell reclaim chain — `collectBoxCaptureLocals`,
// `collectDefinitelyEscapingLambdas`, `collectCellFreeEligible` and their
// `boxCaptureLocalsFn_` / `cellFreeEligibleFn_` tokens — for the same
// `resumeFn` (LlvmBackend_Async.cpp ~L3294-3306). With both halves in place a
// capturing lambda in an async-iterator body is clean, but no test asserts it:
// the existing file is pinned to the non-capturing shape by construction, so
// either half could regress without a red row. These rows close that gap.
//
// ── MEASURED (this file's row 1 program, 500 enumerations, generated-code
// ASan/LSan). With the closure-release arming neutralised in
// `emitAsyncIteratorMethod`: `SUMMARY: AddressSanitizer: 26000 byte(s) leaked
// in 1500 allocation(s)` — 20000 B / 500 direct in `bz$asynciter$M.C$Gen$Resume`
// plus the 4000 B and 2000 B indirect blocks that hang off it. With HEAD
// unmodified: 0 B / 0 allocations. That is the non-vacuity proof for these
// rows; without it a clean LSan run is indistinguishable from an LSan that is
// not armed (see the standing note on one-shot leak tests passing vacuously,
// which is also why every row here drives 500 activations rather than one).
//
// ── DIRECTION OF DANGER. The reclaim these rows depend on is an ARMED release,
// so the failure mode to guard is an over-release, not only a leak — the
// async-iterator path has produced use-after-frees before when a reclaim was
// armed on a path that also disposes elsewhere. Every row is `runAotChecked`
// (ASan + LSan), so a double free or a use-after-free fails the row before its
// printed value is compared, and each row's printed accumulator is an
// independent oracle on the values the closure actually computed.
// per §27.5, §28.9, §30.7, §31.2 (issue #3553)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan with LeakSanitizer
// on: a leak, a double free or a use-after-free fails the row before the
// printed value is compared.
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── Row 1: a CAPTURING sync lambda local in an `IAsyncEnumerable<int>` body,
// 500 enumerations. This is the shape the closure cell, its env block AND its
// capture cell all hang off, so it is red if either #3553's arming or #3507's
// capture-cell chain regresses.
// acc = sum over j in 0..<500 of ((j+1) + (j+2)) = 2*sum(j) + 1500 = 251000.
// per §27.5, §28.9, §30.7 (issue #3553)
BEZEL_TEST(async_iterator_lambda_closure_3553,
           capturing_lambda_in_an_async_iterator_leaks_nothing_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "import System.Threading.Tasks; "
        "class C "
        "  public static async method Gen(n: int): IAsyncEnumerable<int> "
        "    var f := lambda(x: int) => x + n; "
        "    yield return f(1); "
        "    yield return f(2); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var acc := 0; "
        "    for var j in 0..<500 do "
        "      await for var v in C.Gen(j) do acc := acc + v; end; "
        "    end; "
        "    Console.WriteLine(\"acc=\" + acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "acc=251000\n");
}

// ── Row 2: the same capture through an ASYNC lambda, awaited in the iterator
// body. The issue reported both spellings leaking, and an async lambda adds its
// own state machine on top of the closure — so this row says the reclaim is
// keyed to the closure allocation and not to anything about the lambda's own
// suspension. Same accumulator as row 1.
// per §27.5, §30.6, §30.7 (issue #3553)
BEZEL_TEST(async_iterator_lambda_closure_3553,
           capturing_async_lambda_in_an_async_iterator_leaks_nothing_spec_30_6) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "import System.Threading.Tasks; "
        "class C "
        "  public static async method Gen(n: int): IAsyncEnumerable<int> "
        "    var f: (int) -> Task<int> := async lambda(x: int) => x + n; "
        "    yield return await f(1); "
        "    yield return await f(2); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var acc := 0; "
        "    for var j in 0..<500 do "
        "      await for var v in C.Gen(j) do acc := acc + v; end; "
        "    end; "
        "    Console.WriteLine(\"acc=\" + acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "acc=251000\n");
}

// ── Row 3, the SYNC-ITERATOR twin (control): the byte-identical capturing body
// through `emitIteratorMethod`, which has keyed both families to `moveNextFn`
// all along. Clean throughout the issue's history — it is what localises the
// defect to the async-iterator resume fn rather than to iterators generally.
// per §27.5, §28.9 (issue #3553)
BEZEL_TEST(async_iterator_lambda_closure_3553,
           sync_iterator_twin_of_the_capturing_body_stays_clean_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class C "
        "  public static method Gen(n: int): IEnumerable<int> "
        "    var f := lambda(x: int) => x + n; "
        "    yield return f(1); "
        "    yield return f(2); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<500 do "
        "      for var v in C.Gen(j) do acc := acc + v; end; "
        "    end; "
        "    Console.WriteLine(\"acc=\" + acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "acc=251000\n");
}

// ── Row 4, the NO-LAMBDA control: the identical async iterator with the lambda
// replaced by the arithmetic it performed. Clean throughout — it is what says
// the leak was the closure and not the async-iterator frame itself, and it is
// the row that would go red if a future change over-reclaimed the frame.
// per §30.7 (issue #3553)
BEZEL_TEST(async_iterator_lambda_closure_3553,
           async_iterator_without_a_lambda_stays_clean_spec_30_7) {
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
        "    for var j in 0..<500 do "
        "      await for var v in C.Gen(j) do acc := acc + v; end; "
        "    end; "
        "    Console.WriteLine(\"acc=\" + acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "acc=251000\n");
}
