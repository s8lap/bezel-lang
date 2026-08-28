// Codegen — issue #3498 (§28.14 Callback parameters, §27.5 Closures, §28.9
// Runtime-managed types, spec v0.283): a lambda LITERAL passed as an ordinary
// call argument never had its CAPTURE CELLS reclaimed, so an in-loop callback
// leaked one cell per invocation of the enclosing method.
//
// ── THE ISSUE'S ROOT CAUSE IS REFUTED, AND THAT IS WHERE THE FIX LIVES. #3498
// says the trigger is that "a lambda whose capture site sits INSIDE A LOOP BODY
// boxes the captured variable through boxLocalSlot's GUARDED path, and
// lowerLambda's scope-end cell-free admission excludes that shape
// (loopStack_.empty())". Two measured discriminators say that is at most half of
// it, and that neither half is necessary for the other:
//
//   * THE LOOP IS NOT NECESSARY. `using var box := new Box(); s.Take(lambda () =>
//     do box.Add(1); end);` with no loop anywhere leaked 4000 B / 500 over 500
//     calls on `ef339fa`. What that shape has is the ARGUMENT POSITION.
//   * THE ARGUMENT POSITION IS NOT NECESSARY. `var h := lambda () => do
//     box.Add(1); end; h();` inside a `while`, over a `using var` declared
//     OUTSIDE it, leaked 4000 B / 500 too — a directly-bound, invoked, provably
//     non-escaping lambda. What that shape has is the in-loop guarded exclusion.
//
// So there are two independent gaps, each sufficient on its own, and #3498's own
// rows are conjunctions of them. Both are fixed here.
//
// ── GAP 1 (the argument position): §28.14's CALL-BOUNDED DEFAULT was never fed
// to the cell walker. §28.14 makes a function-typed parameter declared without
// `scoped` call-bounded — "the callee receives the callback for the duration of
// the call only … These obligations are compiler-verified for user-authored
// callees" — and `MethodOwnership::callBoundedScan` runs that verification on
// every user body. #1073 already RELIES on exactly this to drop the producer's
// +1 on the CLOSURE after the call. But `collectCellFreeEligible`'s LambdaFinder
// classified any lambda not directly initialising a `var` as ESCAPING, so the
// captures landed in `capturedByEscaping` and the cell was reclaimed by nobody.
//
// The fix is the THIRD non-escaping admission in that walker, beside #2182's
// IIFE and #2334's stdlib `scoped` register positions — and, exactly as #2182
// records for the IIFE, it needs BOTH halves: the pre-scan admission AND a
// `consumeLastLambdaCapturedCells()` at the call site, because `lowerLambda`'s
// entries are dropped on the floor unless someone consumes them and an ordinary
// call was not one of the five consumers.
//
// ── GAP 2 (the in-loop guarded exclusion): that gate was answering a question
// its own consumer answers better. The hazard it names, in its own words, is "a
// local declared in an OUTER scope … captured by an inner-LOOP lambda … freeing
// it at the lambda scope would corrupt later iterations" — but for exactly that
// shape `consumeLastLambdaCapturedCells` never frees at the lambda scope: it
// DEFERS into the owner's scope (#966 / #1017) or SKIPS outright (#969). The
// admission now reads #969's own `declScopeDepth_` predicate, so the shape the
// exclusion really protects — a local at the SAME dispose depth as an in-loop
// capture site, whose one guarded cell would be freed once per iteration — is
// untouched.
//
// ── DIRECTION OF DANGER. Both gaps arm FREES. Under-admitting leaks;
// over-admitting is a use-after-free through a callback the callee kept. Every
// clause of the new §28.14 predicate is a SOURCE-WRITTEN declaration shape and
// fails closed, and the predicate is a strict UNDER-approximation of the one
// #1073 already uses at the same call site — it additionally requires the callee
// to declare NO return type at all, which excludes §28.14's *Call-bounded ×
// deferred activation* (async, iterator, frame-returning) three ways over. That
// containment is the soundness argument: every position it admits is one where
// #1073 demonstrably already releases the closure at the call, so the closure
// cannot outlive the frame.
//
// FAIL-WITHOUT / PASS-WITH, measured program-by-program with
// `bezelc --sanitize=address` (LeakSanitizer on), one build of each compiler:
//
//   program                                        pre-fix (ef339fa)  post-fix
//   #3498 in-loop callback, `using var` OUTSIDE
//     the loop, x500 calls                         4000 B / 500       clean
//   #3498 second shape: `using var` INSIDE the
//     loop (a FRESH cell per iteration), 50 iters   400 B / 50        clean
//   the argument position with NO loop at all,x500 4000 B / 500       clean
//   a directly-bound in-loop lambda, no argument
//     position anywhere, x500                      4000 B / 500       clean
//   a for-var captured by a callback, 100x50 iters 20400 B / 5100      400 B / 100
//   a callee that never REFERENCES the callback,
//     x500                                         4000 B / 500       clean
//   PARITY: an `int` local read after the loop      4 B / 1            4 B / 1
//
// The for-var row does not reach zero and the parity row does not move: both
// residues are the SAME remaining shape — a captured local with no
// reads-through-cell disposal, declared shallower than the capture site, which
// #969 skips because it is read after the capturing scope. Closing it needs a
// fourth mechanism (deferring the free into the DECLARING scope) and is filed
// separately. The rows are kept, with their exact counts, so the next lane can
// see the residue move.
//
// ⚠ THE LEAK ROWS NEED THE ASan/LSan ORACLE (`runAotChecked`); a plain build
// reports an unreclaimed 8-byte cell as a perfectly correct answer. The PARITY
// row is deliberately a plain `runAot` VALUE row, because an LSan row there
// would enshrine the residue as a contract.
// per §28.14, §27.5, §28.9 (issue #3498)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
std::string plainOut(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
// The two classes every row below shares: an owned receiver the callback
// mutates, and a callee with a §28.14 CALL-BOUNDED function-typed parameter
// (no modifier, no declared return type).
constexpr const char* kPrelude =
    "module M; import System; "
    "class Box "
    "  public V: int := 0; "
    "  public method Add(x: int) V := V + x; end Add; "
    "end Box; "
    "class Sink "
    "  public method Take(h: () -> ()) h(); end Take; "
    "end Sink; ";
}  // namespace

// ── The issue's exact first repro, amplified to 500 calls of `Step`. The `using
// var` is declared OUTSIDE the `while`, so the capture takes boxLocalSlot's
// GUARDED path and ONE cell backs all 50 iterations' closures — hence 8 B per
// CALL, not per iteration. Needs BOTH gaps closed. Printed total: 500 x 50.
// per §28.14, §27.5, §28.9 (issue #3498)
BEZEL_TEST(call_bounded_lambda_arg_cell_reclaim_3498,
           in_loop_callback_over_outer_using_var_reclaimed_spec_28_14_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(std::string(kPrelude) +
        "class C "
        "  public static method Step(): int "
        "    using var box := new Box(); "
        "    using var s := new Sink(); "
        "    var i := 0; "
        "    while i < 50 do "
        "      s.Take(lambda () => do box.Add(1); end); "
        "      i := i + 1; "
        "    end; "
        "    return box.V; "
        "  end Step; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<500 do acc := acc + C.Step(); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "25000\n");
}

// ── The issue's SECOND shape: the `using var` is declared INSIDE the loop body,
// so the store sits in the capture site's own basic block, boxLocalSlot takes the
// FRESH path, and a NEW cell is allocated every iteration — 8 B per ITERATION
// (400 B / 50 pre-fix, from the issue). This one needs only gap 1; keeping both
// shapes is what proves the two gaps are separable.
// per §28.14, §27.5, §28.9 (issue #3498)
BEZEL_TEST(call_bounded_lambda_arg_cell_reclaim_3498,
           per_iteration_fresh_cell_callback_reclaimed_spec_28_14_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(std::string(kPrelude) +
        "class C "
        "  public static method Step(): int "
        "    using var s := new Sink(); "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 50 do "
        "      using var box := new Box(); "
        "      s.Take(lambda () => do box.Add(1); end); "
        "      total := total + box.V; "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Step; "
        "  public static method Main(): int "
        "    Console.WriteLine(C.Step().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "50\n");
}

// ── DISCRIMINATOR 1, refuting the filed root cause's "inside a loop body": the
// same argument-position leak with NO loop anywhere in `Step`. 4000 B / 500 on
// `ef339fa`. If the mechanism had really been the in-loop exclusion, this
// program would have been clean before the fix. It was not.
// per §28.14, §27.5, §28.9 (issue #3498)
BEZEL_TEST(call_bounded_lambda_arg_cell_reclaim_3498,
           callback_argument_with_no_loop_reclaimed_spec_28_14_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(std::string(kPrelude) +
        "class C "
        "  public static method Step(): int "
        "    using var box := new Box(); "
        "    using var s := new Sink(); "
        "    s.Take(lambda () => do box.Add(1); end); "
        "    return box.V; "
        "  end Step; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<500 do acc := acc + C.Step(); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "500\n");
}

// ── DISCRIMINATOR 2, refuting the other half: an in-loop lambda that is bound to
// a `var` and INVOKED — no argument position anywhere, so §28.14 is not involved
// at all — over a `using var` declared outside the loop. 4000 B / 500 on
// `ef339fa`; this is gap 2 alone. Note its no-loop twin was already CLEAN before
// the fix (measured), which is what identifies the loop as this row's trigger.
// per §27.5, §28.9 (issue #3498)
BEZEL_TEST(call_bounded_lambda_arg_cell_reclaim_3498,
           in_loop_directly_bound_lambda_over_outer_using_var_reclaimed_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(std::string(kPrelude) +
        "class C "
        "  public static method Step(): int "
        "    using var box := new Box(); "
        "    var i := 0; "
        "    while i < 50 do "
        "      var h := lambda () => do box.Add(1); end; "
        "      h(); "
        "      i := i + 1; "
        "    end; "
        "    return box.V; "
        "  end Step; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<500 do acc := acc + C.Step(); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "25000\n");
}

// ── A FOR-VAR captured by a callback. #793 frees a for-var's FRESH per-iteration
// cell at the end of the iteration body, but only when `collectCellFreeEligible`
// proves the for-var is captured solely by non-escaping lambdas — which, before
// this fix, an argument lambda never was. 20400 B / 5100 allocations pre-fix
// (one cell per iteration over 100 x 50 iterations, plus one per call), 400 B /
// 100 after: the per-iteration leak is gone and what remains is the `total`
// residue this fix deliberately does not reach. Printed total: 100 x sum(0..49).
// per §28.14, §27.5, §28.9 (issue #3498)
BEZEL_TEST(call_bounded_lambda_arg_cell_reclaim_3498,
           for_var_captured_by_callback_frees_per_iteration_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(plainOut(std::string(kPrelude) +
        "class C "
        "  public static method Step(): int "
        "    using var s := new Sink(); "
        "    var total := 0; "
        "    for var i in 0..<50 do "
        "      s.Take(lambda () => do total := total + i; end); "
        "    end; "
        "    return total; "
        "  end Step; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<100 do acc := acc + C.Step(); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "122500\n");
}

// ── The callee never REFERENCES its callback parameter. §28.14's call-bounded
// guarantee does not depend on the callee doing anything with it, and neither
// does this fix — the admission is a property of the DECLARATION, not of the
// body. 4000 B / 500 pre-fix. This row also pins that the closure the caller
// releases (#1073) and the cell this fix frees are two different allocations:
// #1073 was already dropping the closure here, and the cell still leaked.
// Printed total: 500 x 2 (only `box.Add(2)` runs; the callback never does).
// per §28.14, §27.5, §28.9 (issue #3498)
BEZEL_TEST(call_bounded_lambda_arg_cell_reclaim_3498,
           callee_that_ignores_the_callback_still_reclaims_the_cell_spec_28_14) {
    BEZEL_EXPECT_EQ(checkedOut(std::string(kPrelude) +
        "class Holder "
        "  public method Keep(h: () -> ()) end Keep; "
        "end Holder; "
        "class C "
        "  public static method Step(): int "
        "    using var box := new Box(); "
        "    using var k := new Holder(); "
        "    k.Keep(lambda () => do box.Add(1); end); "
        "    box.Add(2); "
        "    return box.V; "
        "  end Step; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<500 do acc := acc + C.Step(); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1000\n");
}

// ── PARITY / RESIDUE row: #3498's third shape. A plain `int` local declared
// OUTSIDE the loop, captured by an in-loop callback, and READ AFTER the loop. It
// has no reads-through-cell disposal, so `consumeLastLambdaCapturedCells` cannot
// defer the free into the owner's scope, and #969 skips it outright — correctly,
// because the cell is read after the capturing scope. 4 B / 1 allocation, the
// SAME before and after this commit; closing it needs a fourth mechanism
// (deferring the free into the DECLARING scope) and is filed separately.
//
// Kept as a plain VALUE row so the residue is not enshrined as an LSan contract,
// and because it is the row that would go red if a future widening of #969's
// skip freed the cell before `return total` reads it.
// per §27.5, §28.9 (issue #3498)
BEZEL_TEST(call_bounded_lambda_arg_cell_reclaim_3498,
           residue_int_local_read_after_the_capturing_loop_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(plainOut(std::string(kPrelude) +
        "class C "
        "  public static method Step(): int "
        "    using var s := new Sink(); "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 50 do "
        "      s.Take(lambda () => do total := total + 1; end); "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Step; "
        "  public static method Main(): int "
        "    Console.WriteLine(C.Step().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "50\n");
}
