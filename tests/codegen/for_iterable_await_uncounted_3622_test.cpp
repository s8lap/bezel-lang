// Codegen — issue #3622 (§21.5 for-statement / §30.2 / §30.7 / §29.2 / §30.5 /
// §28.3): `ast::ForStmt::iterable` (`for x in EXPR do`) was skipped by both
// statement-level "does this contain an await?" walkers in
// LlvmBackend_Async.cpp (`Emitter::containsAwait` / the file-static
// `countAwaits`) — the resume switch had no state block for an await there,
// so `for var x in (await Producer()) do ... end` hung (the #441/#2226 shape,
// `timeout 10` -> exit 124).
//
// #3587 closed the sibling `CaseExpr` gap (#2251) but left `fs->iterable`
// deliberately uncounted: the filing lane tried the obvious count-only patch
// and MEASURED it turning the deterministic hang into a NONDETERMINISTIC
// crash / silent-wrong-value race (5 runs: exit 139 x4, exit 0 x1 printing 0
// instead of 6) — a loud->quiet trade CLAUDE.md §6c rejects. That lane
// correctly declined to ship half the fix and filed #3622 for the pair.
//
// ROOT CAUSE (re-derived here; the filing issue's own hypothesis — a
// spill/dominance defect in the for-loop's iteration-state setup,
// LlvmBackend_Stmts.cpp — was UNVERIFIED and turned out to be wrong). The
// real defect is a disposal-TIMING bug in the UNRELATED #3070 mechanism
// (`Emitter::lowerAwait`'s `deferTaskFree` / `registerAwaitedTaskTemp` /
// `flushAwaitedTaskTemps`, LlvmBackend_Async.cpp): an awaited anonymous
// Pattern A producer (`await Producer()` for a `Task<T>` where T is a class
// or intrinsic collection) defers `bz_task_free` to the enclosing K8
// full-expression boundary so the borrowed result stays valid for the rest
// of that expression (§30.5). But `lowerExpr`'s K8-boundary hook explicitly
// treats a `for` SOURCE as its own top-level expression (LlvmBackend_Exprs.cpp
// header comment: "every statement expression, initializer, condition, `for`
// source and `case` scrutinee reaches lowering as a fresh top-level call") —
// so for `for x in (await GetArr()) do ... end`, the boundary fires the
// instant the SOURCE finishes evaluating, freeing the task (and, via its
// Pattern A result_disposer, #1503, the array it owns) BEFORE the loop body
// ever reads it. §30.5's "disposed ... after every use of the borrow" simply
// does not hold when the borrow's uses (the loop body) live outside that
// boundary.
//
// THE FIX (two halves, landed together as #3622 requires):
//   1. `containsAwait` / `countAwaits` now scan `fs->iterable` (the naive
//      half #3587 declined to ship alone).
//   2. `lowerAwait` computes `deferToScopeOnly` by asking whether the current
//      await is reachable from `deferredRecvScopeBoundInit_` (fix #3377's
//      existing "for-in iterable currently being lowered" flag) via
//      `bezel::common::forEachAwaitSite` (fix #3587's shared await-site walk
//      — reused rather than re-derived, CLAUDE.md §6). When true,
//      `registerAwaitedTaskTemp` withholds the K8-boundary queue entry and
//      leaves the reclaim to the ordinary `disposeStack_` scope-end pass,
//      which — because the enclosing block scope necessarily contains the
//      WHOLE for-statement (source and body) — cannot fire before the loop
//      finishes. A for-loop BODY await is a disjoint AST subtree the walk
//      never visits, so per-iteration K8 reclaim there (§30.5
//      "loop_body_reclaims_per_iteration", pinned by
//      await_unbound_task_k8_boundary_3070_test.cpp) is unaffected.
//
// §6b EXCLUSION DIFF (the ONE walk, `bezel::common::forEachAwaitSite` /
// `Emitter::containsAwait` / `countAwaits`, is unchanged by this fix in its
// own right — this fix is about a DIFFERENT walk, the ForStmt::iterable scan
// these two ALREADY had a dedicated arm for but intentionally skipped):
//   OLD: `fs->iterable` excluded unconditionally from both walkers, for every
//        for-loop shape (array-backed, dict-backed, IEnumerable-backed,
//        range, string, span, ...) — regardless of whether an await inside
//        it would ever reach the (until now unsafe) lowering path.
//   NEW: `fs->iterable` admitted unconditionally, for the same set of shapes
//        — via `exprContainsAwait`/`countAwaitsInExpr`, the SAME shared
//        `forEachAwaitSite`-backed helpers every other position already
//        uses, so no new walker/duplicate predicate was added (CLAUDE.md
//        §6). The accompanying `deferToScopeOnly` gate is centralized in
//        `lowerAwait`/`registerAwaitedTaskTemp` — NOT threaded through the
//        ~14 individual for-loop lowering functions in
//        LlvmBackend_Stmts.cpp (lowerArrayFor, lowerIEnumerableBuiltinFor,
//        lowerIEnumerableFor, lowerDictBackedSetFor, lowerStringFor,
//        lowerStringSpanFor, lowerLazyLinqFor, lowerOwnedSeqDrainFor,
//        lowerDictDrainFor, lowerLinkedListFor, lowerRangeFor, ...), each of
//        which independently calls `lowerExpr(*fs.iterable)` — because the
//        gate is a purely AST-structural question ("is this await reachable
//        from the currently-in-flight for-source expression?") answered the
//        same way regardless of which handler eventually consumes the
//        result, so it needed no per-shape special-casing and could not
//        silently miss one.
//
// A SEPARATE, PRE-EXISTING DEFECT DISCOVERED (NOT fixed here, filed
// separately — see the issue tracker): a PLAIN (non-`using`) `var arr :=
// await Producer();` followed by a LATER statement using `arr`, where
// Producer() returns a Pattern A class/collection, has the SAME
// use-after-free race — `deferredRecvScopeBoundInit_` is null for a plain
// var (only `using var` and for-in iterables set it), so the K8 boundary
// still fires at the LocalDecl's own initializer expression, before the
// later statement runs. Out of #3622's scope (LocalDecl, not ForStmt), and
// arguably a sema/borrow-checking gap (§28.3 "confined to the same full
// expression" — a plain var escaping that expression may need to be REJECTED
// rather than fixed in codegen).
//
// FAIL-WITHOUT / PASS-WITH (measured on this fix, HEAD d8e07d7):
//   Base (both arms uncounted): the repro below HANGS — `timeout 10` -> exit
//     124, stable.
//   Count-only (the rejected half): 8 runs, `timeout 10` — 3x SIGSEGV (exit
//     139), 5x exit 0 (WRONG VALUE — the correct answer is 6).
//   Both halves (this fix): 15 plain-oracle runs and 5 ASan/LSan runs, all
//     exit/return exactly 6, no sanitizer report.

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

using bezel::test::runAotChecked;

namespace {
namespace fs_3622 = std::filesystem;

const std::filesystem::path& wtDir3622() {
    return wtScratchDir("wt-3622");
}

// Same shape as async_await_extra_kinds_2226_test.cpp's runWithTimeout: the
// AOT harness's own default timeout (30s, aotTimeoutSec()) is too generous
// for a per-run hang probe amplified over many iterations, so this drives
// `Driver::compileFile` + a 5s `timeout` directly. Returns the exit code, or
// -3 on a signal/timeout kill (a hang).
int runWithTimeout3622(const std::string& source) {
    auto dir = wtDir3622();
    std::string stem = unique_stem("bezel_for_iter_await_3622");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    {
        std::ofstream out(src);
        out << source;
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    if (!r.success) {
        std::remove(src.c_str());
        std::remove(exe.c_str());
        return -2;  // compile failure — would mask a hang; never expected here
    }
    std::string cmd = "timeout 5 " + exe + " > /dev/null 2>&1";
    int status = std::system(cmd.c_str());
    std::remove(src.c_str());
    std::remove(exe.c_str());
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -3;  // killed by signal (timeout's SIGTERM/SIGKILL) -> hang
}

// Runs the program N times and asserts every run produces the expected exit
// code. N=10, not the usual 3x: #3622's specific pre-full-fix failure mode is
// a nondeterministic RACE (measured 3/8 crash, 5/8 silent wrong value, 0/8
// correct) — a low iteration count could get lucky and pass by chance.
void expectStable3622(const std::string& source, int expectedExitCode,
                      int n = 10) {
    for (int i = 0; i < n; ++i) {
        int got = runWithTimeout3622(source);
        if (got != expectedExitCode) {
            std::fprintf(stderr,
                "for_iterable_await_uncounted_3622: run %d expected=%d got=%d\n",
                i, expectedExitCode, got);
        }
        BEZEL_EXPECT_EQ(got, expectedExitCode);
    }
}

int runChecked3622(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}

// `GetArr` forces a REAL cross-thread resume (Task.Delay(40) — synchronous
// completion would mask the bug the same way #2226's header notes) and
// returns a Pattern A collection result (Array<int> via a `[...]` literal),
// exactly the shape #3070's result_disposer cascades through.
const char* kGetArr =
    "class C "
    "  public static async method GetArr(): Task<Array<int>> "
    "    await Task.Delay(40); "
    "    return [1, 2, 3]; "
    "  end GetArr; ";

// Self-contained (closed) twin of kGetArr, for a program that needs GetArr in
// its own class rather than opened for a caller to append Main into.
const char* kGetArrClosed =
    "class C "
    "  public static async method GetArr(): Task<Array<int>> "
    "    await Task.Delay(40); "
    "    return [1, 2, 3]; "
    "  end GetArr; "
    "end C; ";
}  // namespace

// ── The issue's own repro: await directly in the for-in source. ─────────────
// Sum of [1, 2, 3] = 6. per §21.5 / §30.2 / §30.7 (issue #3622)
BEZEL_TEST(for_iterable_await_uncounted_3622, await_producer_in_iterable_position) {
    expectStable3622(
        std::string("module M; ") + kGetArr +
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var x in (await GetArr()) do "
        "      total := total + x; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;",
        6);
}

// ── Same repro, ASan/LSan-instrumented: the naive count-only fix's failure
// mode was a heap use-after-free (the array freed by the awaited task's
// Pattern A result_disposer before the loop read it) — this leg proves the
// full fix is memory-clean, not merely value-correct by luck. per §30.5 /
// §28.3 (issue #3622)
BEZEL_TEST(for_iterable_await_uncounted_3622, await_producer_in_iterable_position_asan) {
    for (int i = 0; i < 5; ++i) {
        BEZEL_EXPECT_EQ(runChecked3622(
            std::string("module M; ") + kGetArr +
            "  public static async method Main(): Task<int> "
            "    var total := 0; "
            "    for var x in (await GetArr()) do "
            "      total := total + x; "
            "    end; "
            "    return total; "
            "  end Main; "
            "end C;"),
            6);
    }
}

// ── The BODY's own per-iteration K8 reclaim must be UNCHANGED by this fix:
// an unrelated Pattern A await INSIDE the loop body (not the iterable) must
// still be disposed per-iteration (§30.5's `loop_body_reclaims_per_iteration`
// contract, pinned separately by await_unbound_task_k8_boundary_3070_test.cpp
// / kLoopPerIteration), not deferred to the whole for-statement's enclosing
// scope end — `deferredRecvScopeBoundInit_` stays set across the loop body
// (fix #3377), so this proves the `forEachAwaitSite`-based membership check
// correctly EXCLUDES body awaits from `deferToScopeOnly`, not merely that it
// includes iterable ones.
//
// The body await's borrow is consumed WITHIN THE SAME FULL EXPRESSION
// (`(await D.MakeCell(...)).v`), mirroring kMemberAccess/kLoopPerIteration in
// await_unbound_task_k8_boundary_3070_test.cpp exactly — NOT stored into a
// named local for a later statement to read. (A plain, non-`using` local
// bound to a Pattern A await and read in a LATER statement is a separate,
// pre-existing defect this fix does not touch: `deferredRecvScopeBoundInit_`
// is null for such a local, so its own K8 boundary still fires at ITS OWN
// initializer, same as before this fix — filed separately, not #3622.)
//
// `Cell.n` (a live-instance counter), checked INSIDE the loop right after
// each use, makes "disposed by the end of THIS iteration's expression, not
// accumulated to loop end" observable per-iteration: a leak surfaces as
// `sum := -1` on the very iteration it first goes stale, distinguishable
// from every correct partial sum (10, 30, 60). per §30.5 "Result lifetime
// and task disposal" (issue #3622)
BEZEL_TEST(for_iterable_await_uncounted_3622, body_await_reclaims_per_iteration_not_deferred_to_loop_end) {
    expectStable3622(
        std::string("module M; ") + kGetArrClosed +
        "class Cell "
        "  public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class D "
        "  public static async method MakeCell(k: int): Task<Cell> "
        "    return new Cell(k); "
        "  end MakeCell; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    for var x in (await C.GetArr()) do "
        "      sum := sum + (await D.MakeCell(x * 10)).v; "
        "      if Cell.n <> 0 then return -1; end; "
        "    end; "
        "    return sum; "
        "  end Main; "
        "end D;",
        60);
}
