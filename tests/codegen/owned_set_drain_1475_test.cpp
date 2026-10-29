// Codegen — fix #1475 / stdlib §4.10 / spec §29.2: `for var x in
// ownedSet.Drain()` — ownership-transferring drain iteration over
// OwningSet<T> (a class-typed T, per the `where T: class` constraint stdlib
// §4.10 places on OwningSet<T>).
//
// Root cause was two-layered:
//   1. sema (ExprTypecheck_Calls.cpp): the "Drain() returns
//      IOwningEnumerable<T>" typing arm only matched OwningArray/
//      OwningLinkedList/OwningStack/OwningQueue — OwningSet was missing, so
//      `s.Drain()`'s expr type was left unset (neither IOwningEnumerable<T>
//      nor anything else), and the for-loop's iterable never matched the
//      IOwningEnumerable<T> shape codegen looks for.
//   2. codegen (LlvmBackend_Stmts.cpp, lowerOwnedSeqDrainFor): even with the
//      sema fix, the collection-kind allowlist that picks the runtime drain
//      entry point excluded "OwningSet".
// Both gaps are now closed: OwningSet<T> (T: class, per #1474 always
// ARRAY-backed — see owned_set_union_1474_test.cpp) reuses the exact same
// bz_array_drain_next entry point OwningArray/OwningQueue use, since its
// runtime representation is the identical bz_array_t.
//
// Before the fix, the repro's `for var x in s.Drain() do ... end` failed to
// COMPILE at all: "not yet implemented: codegen for this for statement"
// (spurious-error, not a runtime bug) — every test below is load-bearing
// against that regression, plus the drain-loop's ownership obligations
// (exactly-once disposal, Count == 0 after full drain, partial-drain
// remainder still disposed by the set's own Dispose()).

// AOT MIGRATION (#1769 / #1763): this file runs under the AOT driver (runAot).
// The drain loop behaves identically under AOT (verified — the JIT→AOT
// divergence #1769 flagged for this file was purely the 8-bit exit channel: the
// drain-visits test encodes a 306 observable, which masks to 50 through a
// process exit status). That test prints the observable with Console.WriteLine
// (stdlib §2.1) and asserts the captured stdout; the ≤255 observables round-trip
// the exit code.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Shared destructor-counter class template (mirrors owned_drain_639_test.cpp's
// `R` / makeProgram helper).
std::string makeProgram(const std::string& mainBody) {
    return
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        + mainBody + " end Main; end C;";
}
}  // namespace

// stdlib §4.10 / spec §29.2: the exact #1475 repro — must compile and run
// (previously a hard codegen error). An empty set drains zero iterations and
// exits cleanly.
BEZEL_TEST(owned_set_drain_1475, empty_set_drain_compiles_and_runs_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Cc "
        "public static method Main(): int "
        "  using var s := new OwningSet<Cc>(); "
        "  for var x in s.Drain() do "
        "    using var _ := move x; "
        "  end; "
        "  return 0; "
        "end Main; end Cc;"), 0);
}

// §4.10: full drain disposes every element exactly once.
BEZEL_TEST(owned_set_drain_1475, full_drain_disposes_all_spec_4_10) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var s := new OwningSet<R>(); "
        "s.Add(move (new R())); "
        "s.Add(move (new R())); "
        "s.Add(move (new R())); "
        "for var item in s.Drain() do "
        "  using var u := move item; "
        "end; "
        "return R._n;"  // 3
    )), 3);
}

// §4.10: after full drain the set is empty (Count == 0).
BEZEL_TEST(owned_set_drain_1475, full_drain_empties_set_spec_4_10) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var s := new OwningSet<R>(); "
        "s.Add(move (new R())); "
        "s.Add(move (new R())); "
        "for var item in s.Drain() do "
        "  using var u := move item; "
        "end; "
        "return s.Count;"  // 0
    )), 0);
}

// §4.10: the loop visits every element exactly once — three distinct
// elements added, three seen by the loop body.
BEZEL_TEST(owned_set_drain_1475, drain_visits_every_element_once_spec_4_10) {
    auto r = runAot(
        "module M; "
        "sealed class Val "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Val; "
        "class C public static method Main(): int "
        "  using var s := new OwningSet<Val>(); "
        "  s.Add(new Val(1)); "
        "  s.Add(new Val(2)); "
        "  s.Add(new Val(3)); "
        "  var seen := 0; "
        "  var sum := 0; "
        "  for var item in s.Drain() do "
        "    using var u := move item; "
        "    seen := seen + 1; "
        "    sum := sum + u.N; "
        "  end; "
        // 3*100 + (1+2+3) = 306 > 255 → stdout.
        "  Console.WriteLine(seen * 100 + sum); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "306\n");
}

// §4.10 / spec §29.7: partial drain (break after the first iteration) — the
// remaining elements stay in the set and are disposed by the set's own
// Dispose() at scope end, exactly once each (no leak, no double dispose).
BEZEL_TEST(owned_set_drain_1475, partial_drain_break_remainder_disposed_spec_4_10) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "var afterBreak := -1; "
        "do "
        "  using var s := new OwningSet<R>(); "
        "  s.Add(move (new R())); "
        "  s.Add(move (new R())); "
        "  s.Add(move (new R())); "
        "  for var item in s.Drain() do "
        "    using var u := move item; "
        "    break; "
        "  end; "
        "  afterBreak := R._n; "  // 1 disposed so far
        "end; "
        "return afterBreak * 10 + R._n;"  // 1*10 + 3 = 13
    )), 13);
}

// §4.10: empty set drain — loop body never executes.
BEZEL_TEST(owned_set_drain_1475, empty_set_drain_no_iterations_spec_4_10) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var s := new OwningSet<R>(); "
        "var count := 0; "
        "for var item in s.Drain() do "
        "  using var u := move item; "
        "  count := count + 1; "
        "end; "
        "return count;"  // 0
    )), 0);
}

// §4.10: the set is reusable after a full drain (Add works, Count increases).
BEZEL_TEST(owned_set_drain_1475, reusable_after_full_drain_spec_4_10) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var s := new OwningSet<R>(); "
        "s.Add(move (new R())); "
        "s.Add(move (new R())); "
        "for var item in s.Drain() do "
        "  using var u := move item; "
        "end; "
        "s.Add(move (new R())); "  // add after drain
        "return s.Count;"  // 1 — reusable
    )), 1);
}
