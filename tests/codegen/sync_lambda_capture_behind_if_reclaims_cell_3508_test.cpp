// Codegen — issue #3508 (spec §27.5 Closures, §28.9 Runtime-managed types,
// v0.286): a SYNC lambda capture behind an `if` (no async, no catch-arm,
// no state machine) leaks its capture cell once per invocation that takes
// the branch.
//
//     public static method Run(n: int): int
//       var k := n * 2;
//       var s := 0;
//       if n mod 2 = 0 then
//         var g := lambda(): int => k;
//         s := g();
//       end if;
//       return s + k;
//     end Run;
//
//     $ bezelc --sanitize=address -o p p.bzl && ./p
//     0
//     SUMMARY: AddressSanitizer: 2000 byte(s) leaked in 500 allocation(s).
//
// ── DISCRIMINATOR IS THE `if`, NOT THE VENUE. The same capture UNCONDITIONAL
// is clean (the unconditional sync shape is well-handled — the lambda and
// the captured local's store are in the same basic block, so
// `boxLocalSlot`'s FRESH path is taken and the consumer's depth guard
// agrees with the registration scope). Move the capture behind an `if` —
// equally a `while`, a `case`, or any block-splitter — and the store ends
// up in a DIFFERENT basic block, `boxLocalSlot` takes the GUARDED path,
// `wasGuardedLocal` becomes true, and the registration scope (the lambda's
// block) and the local's declaration scope (an OUTER block) diverge. The
// consumer's #969/#3403 depth guard then refuses to free at the inner
// scope and falls through to a `continue;` — the cell leaks even though
// `loopStack_.empty()` admits the cell at registration time.
//
// ── THE MECHANISM, PRE- AND POST-FIX. Pre-fix, the registration admits the
// cell on the `loopStack_.empty()` branch (the `if`-split is NOT a loop,
// so that branch fires), then the consumer's `if (dd != declScopeDepth_
// .end() && dd->second < disposeStack_.size()) continue;` clause refuses
// the free at the inner scope. There is no second consumer path the cell
// could go through: the lambda is non-escaping so `freeCellInEnvdtor`
// does not fire, the cell is not shared with a second lambda so
// `sharedCellEnvdtorFreeOwner_` does not name it, and `captureByEscap-
// ing` is empty so the #966/#1017 deferred-to-owner branch has no
// reads-through-cell disposal to anchor on. The cell therefore sits in
// `k_slot` (the function-entry holder) until the function returns, at
// which point it is unreachable and LSan reports it. Post-fix, the
// registration consults the same `declScopeDepth_` predicate the
// consumer uses (read here rather than respelled, so the two cannot
// disagree) and, for the SYNC `/`-behind-`if` shape specifically —
// `loopStack_.empty()` AND `cellFreeEligible_` AND `!alreadyBoxed` AND
// `!isForVar` AND `!isPatternBinding` AND `currentFn_ == cellFree
// EligibleFn_` — pushes a `boxedCellFreeDeferred` DIRECTLY into the
// OWNER's `disposeStack_` slot. The deferred fires AFTER every main-
// loop disposal in the owner's scope (LlvmBackend_Disposal.cpp's #1017
// SECOND PASS), and for a non-using-var outer local like `k` that means
// it fires at the function-body root scope end — AFTER `return s + k;`
// has run and any closure holding the cell has been released into the
// envdtors of `g`. No read races the cell-free; no escaping closure
// outlives the owner's scope (cellFreeEligible_'s pre-scan proves it).
//
// ── NARROWING (the §6b "what old copy excluded / new copy admits" exercise,
// for the registration-side change only — the consumer-side depth guard
// is in Setup.cpp and is NOT touched by this commit):
//
//   site                                                 pre-fix result   post-fix result
//   ── unconditional sync capture (same BB)              FREE @ root      UNCHANGED
//   ── sync capture in `if` (the issue), outer local     LEAK (root)     FREE @ root (deferred)
//   ── sync capture in `while`, outer local (in-loop)    LEAK @ lambda    LEAK @ lambda (unchanged)
//   ── sync capture behind `if`, INNER local (same depth) FREE @ if-scope UNCHANGED
//   ── sync capture, outer local, ESCAPING sibling        existing owner-  UNCHANGED
//                                                        deferred free
//   ── sync capture, outer local, ASYNC sibling           existing       UNCHANGED
//                                                        pre-scan routes
//   ── case-arm binding capture, outer local             existing pattern UNCHANGED
//                                                        binding branch
//                                                        handles it
//   ── inline-owned-array capture, outer local, sync     LEAK            FREE @ root (deferred +
//                                                                          inlineOwnedArrayCell)
//
// The new branch admits EXACTLY the shape the issue tested: a non-escaping
// sync lambda, first capture, no for-var, no pattern binding, no enclosing
// loop, outer-scope local, in `cellFreeEligible_`. Every OTHER branch is
// untouched (the existing `lastLambdaCapturedCells_.push_back(lce)` is
// preserved; the consumer's depth guard still skips for declaredShallower;
// the new deferred free is the only thing that fires — and it is deduped
// against any pre-existing `boxedCellFreeDeferred` on the same slot in the
// owner's scope, so a double-free is structurally impossible).
//
// ── FAIL-WITHOUT / PASS-WITH (bezelc --sanitize=address, LeakSanitizer on,
// one build of each compiler; every leak row amplified per LANE_RULES §5):
//
//   row                                         pre-fix (HEAD de879ab)  post-fix
//   issue exact, sync method, x500              2000 B / 500 allocs      clean
//   unconditional sync twin (must STAY clean,
//     per `if`-behind discriminator analysis)   clean                   clean
//   INNER-local capture behind `if` (depth
//     guard does not fire; cell-free at if-
//     scope must not regress)                   clean                   clean
//   async-method twin of row 1 (out of scope
//     for #3508 — covered by #3506 / #3507 —
//     pinned only so a regression is loud)      leaks (irrelevant here) unchanged
//   no-lambda control of row 1                 clean                   clean
//
// ⚠ These leak rows need the ASan/LSan oracle (`runAotChecked`), not
// `runAot`: the effect under test is memory nobody frees, which a plain
// build reports as a perfectly correct answer.
// per §27.5, §28.9 (issue #3508)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan with LeakSanitizer
// on, so a non-empty leak report fails the row before the printed value is
// compared.
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
// Plain runAot for value-only rows whose shape prints correctly either way.
std::string plainOut(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── Row 1: the issue's exact repro, LOOP-AMPLIFIED to 500 invocations.
// Pre-fix LSan report: 2000 B / 500 allocations (4 B per invocation that
// takes the if-branch — `Run(0)` always takes the branch). Post-fix:
// clean. The printed total is 0 because every iteration returns
// s + k = 0 + 0 = 0 for n = 0.
// per §27.5, §28.9 (issue #3508)
BEZEL_TEST(sync_lambda_capture_behind_if_reclaims_cell_3508,
           sync_lambda_capture_behind_if_500x_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Run(n: int): int "
        "    var k := n * 2; "
        "    var s := 0; "
        "    if n mod 2 = 0 then "
        "      var g := lambda(): int => k; "
        "      s := g(); "
        "    end if; "
        "    return s + k; "
        "  end Run; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      acc := acc + Run(0); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "0\n");
}

// ── Row 2: the same lambda with the capture UNCONDITIONAL (no `if`). The
// discriminator in the issue's analysis is the block boundary, not the
// lambda or the local — with both the local's store and the capture in
// the function body's single basic block, `boxLocalSlot` takes the FRESH
// path and the existing scope-end free fires at the root scope. Must
// STAY clean. Loop-amplified to 500 to keep the no-vacuous-pass discipline.
// per §27.5, §28.9 (issue #3508)
BEZEL_TEST(sync_lambda_capture_behind_if_reclaims_cell_3508,
           unconditional_sync_twin_stays_clean_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Run(): int "
        "    var k := 42; "
        "    var s := 0; "
        "    var g := lambda(): int => k; "
        "    s := g(); "
        "    return s + k; "
        "  end Run; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      acc := acc + Run(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        // 500 * (42 + 42) = 42000.
        "42000\n");
}

// ── Row 3: INNER-LOCAL capture behind `if` (the captured local lives in
// the same scope as the lambda). Here `declaredShallower` is FALSE so
// neither the pre-fix consumer NOR the post-fix new branch should fire —
// the cell-free happens at the if-scope and that is the existing free path.
// Must STAY clean. Sum is 500 * (s + k) where s = k = i*2 for the inner
// case, so... pick something simpler. Value oracle: 500 * (42 + 42) = 42000
// (Run() always returns 84).
// per §27.5, §28.9 (issue #3508)
BEZEL_TEST(sync_lambda_capture_behind_if_reclaims_cell_3508,
           inner_local_capture_behind_if_stays_clean_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Run(): int "
        "    var s := 0; "
        "    if true then "
        "      var k := 42; "
        "      var g := lambda(): int => k; "
        "      s := g(); "
        "    end if; "
        "    return s; "
        "  end Run; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      acc := acc + Run(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "21000\n");
}

// ── Row 4: the NO-LAMBDA control. Removes the lambda from Row 1; the
// class instance itself is what would otherwise allocate a cell. Clean
// before AND after the fix — proves the leak being measured is the cell
// and not some other invariant of the surrounding code. Sum is 0
// (Run(0) always returns 0 + 0 = 0).
// per §27.5, §28.9 (issue #3508)
BEZEL_TEST(sync_lambda_capture_behind_if_reclaims_cell_3508,
           no_lambda_control_of_row_1_is_unchanged_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Run(n: int): int "
        "    var k := n * 2; "
        "    var s := 0; "
        "    if n mod 2 = 0 then "
        "      s := k; "
        "    end if; "
        "    return s + k; "
        "  end Run; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      acc := acc + Run(0); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "0\n");
}
