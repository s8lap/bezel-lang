// Codegen — issue #4509 / spec §27.5 + §28.9 (v0.306): a capture CELL reached
// by a CONSERVATIVELY-escaping lambda is reclaimed by refcounting it.
//
// THE DEFECT. A captured local is promoted to a shared heap cell
// (`boxLocalSlot`). Three static routes decide who frees it, and all three
// decline when any capturer is a lambda merely PASSED AS AN ARGUMENT:
//
//   * the scope-end route (`cellFreeEligible_` → `boxedCellFree` /
//     `boxedCellFreeDeferred`) declines because `declsCapturedByEscaping_`
//     (#4210) holds the key — an escaping env holds a raw cell pointer that
//     MAY outlive the frame;
//   * #694's envdtor route needs `definitelyEscapingLambdas_`, whose own
//     condition (b) declines exactly this case: "a conservatively-escaping
//     lambda — one merely passed as an argument — proves nothing about when
//     its env dies";
//   * #3426's shared-cell route needs (b) as well.
//
// So nobody freed it. 8 bytes per capture, unbounded in a loop.
//
// THE FIX. All three declines are the same missing fact — WHEN the escaped env
// dies — which is a runtime quantity, so it is answered at runtime:
// `bz_cell_retain` at each env's capture-store, `bz_cell_release` in that env's
// envdtor and once at the declaring scope's end (a `Disposal::cellRelease` in
// the #1017 SECOND PASS). The last holder out frees the cell, whichever it is.
// This is the cell-only half of the model #2718 sketches; the cell's CONTENTS
// accounting is that issue's separate half and is untouched here.
//
// WHY THIS CANNOT DOUBLE-FREE: it arms only where today's codegen emits NO free
// at all. Adding a balanced retain/release pair to a set with zero frees can
// only leave a leak, never turn one free into two. The rows below pin both
// directions anyway — reclamation AND the absence of an over-release.
//
// MEASURED (generated code uninstrumented over an ASan-instrumented runtime —
// the recipe `event_subscription_closure_841_test.cpp` uses; and again under
// `runAotChecked`, which instruments the generated code so a use-after-free on
// the cell faults AT THE ACCESS):
//
//   200-iteration event-subscription loop .. pre-fix 1592 B / 199 allocs, post-fix clean
//   50-iteration guarded-cell loop ......... pre-fix   12 B /   2 allocs, post-fix clean
//   `this`-bounded env outliving its frame . pre-fix    4 B /   1 alloc,  post-fix clean
//   one-shot ............................... pre-fix    8 B /   1 alloc,  post-fix clean
//
// The pre-fix column is measured by rebuilding `bezelc` with this fix's
// classification disabled (`if (false && …)`) and re-running each program —
// every row above FAILS on that compiler and passes on this one.
//
// The one-shot figure is the only one that needs `LSAN_OPTIONS=use_stacks=0` to
// be visible on the pre-fix compiler — a single iteration can leave the last
// cell pointer discoverable by LSan's conservative stack scan and pass for the
// wrong reason, which is the accident that kept this defect invisible for so
// long (see #4509 and the same shape in #3498). That is why the two volume rows
// here are LOOP-AMPLIFIED. The `this`-bounded row is deliberately one-shot and
// still fails pre-fix on its own (4 B / 1), because its captured cell has no
// reclaim site at all rather than a racy one; amplifying it would add nothing.
//
// ON `Button`'s SPELLING (#1727). `public event Clicked: Ev;` is spec §24
// (v0.306)'s RETIRED member kind — the migration spelling is stdlib §6.5's
// `private owned readonly _clicked: EventSource<Ev> := new();` +
// `public property Clicked: IEvent<Ev> => _clicked;` pattern, but issue #1727
// (the compiler half of the retirement) is still open, so `event` still
// compiles. `Button` here was rewritten to the §6.5 pattern and measured
// AGAINST THIS FIX'S OWN CLAIM: rewritten, the 200-/50-iteration
// `using var sub := b.Clicked.Subscribe(...)` shape (rows 1 and 2) no longer
// leaks AT ALL, pre-fix or post — measured clean (exit 7) under
// `--sanitize=address` / `ASAN_OPTIONS=detect_leaks=1:use_stacks=0` on a
// `bezelc` built at a2f3becb (before #4509's fix existed), versus the
// unrewritten `event` spelling's exact 1592 B / 199 allocations on the same
// binary. A user-authored property getter is evidently visible to the
// compiler's escape classification in a way the retired member kind's
// synthetic accessor is not, so rows 1/2 would no longer be pinning anything
// if rewritten. Per this repo's discipline (never weaken what a regression
// test pins), `Button` STAYS on the retired `event` spelling — it joins the
// #1727 migration pile rather than being fixed here. `Widget` below (used
// only by the `this`-bounded row) WAS converted to the §6.5 pattern: that
// row's defect was reconfirmed to still reproduce post-rewrite (pre-fix at
// a2f3becb: `r.ran` false, wrong `T`; clean and `T == 5` post-fix), so the two
// classes are not interchangeable for this purpose and are handled
// differently on that measured basis, not by assumption.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The §24 event-subscription shape #4509 was found on: a `using var` class
// local captured by a handler lambda passed to `Event<T>.Subscribe`. The
// subscription is bounded by a `using var` in an inner block (§28.14 form 1),
// so the handler's env is released INSIDE the declaring scope — the ordering
// under which the frame is the last holder and its release is the cell's free.
//
// `total == 2 * N` proves every iteration's handler really ran, so the loop is
// amplifying the live shape and not an optimized-away one.
std::string eventSubscriptionLoop(int n) {
    return
        "module M; "
        "record Ev(N: int); "
        "class Box "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box; "
        "class Button "
        "  public event Clicked: Ev; "
        "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
        "end Button; "
        "class C public static method Main(): int "
        "  var total := 0; "
        "  for var i in 0..<" + std::to_string(n) + " do "
        "    using var box := new Box(); "
        "    using var b := new Button(); "
        "    do "
        "      using var sub := b.Clicked.Subscribe(lambda(e) => do box.Add(e.N); end); "
        "      b.Fire(2); "
        "    end; "
        "    total := total + box.V; "
        "  end; "
        "  if total = " + std::to_string(2 * n) + " then return 7; end; "
        "  return 1; "
        "end Main; end C;";
}

}  // namespace

// §27.5 / §28.9 / #4509 — THE REPRO, loop-amplified. Pre-fix this leaked
// 1592 B in 199 allocations at 200 iterations (8 B per capture cell, one per
// iteration); post-fix it is clean. `runAotChecked` instruments the generated
// code, so the same row also fails on a use-after-free or double-free of the
// cell — it pins reclamation and the absence of over-release together.
BEZEL_TEST(escaping_capture_cell_refcount_4509,
           event_subscription_capture_cell_reclaimed_spec_27_5) {
    auto r = runAotChecked(eventSubscriptionLoop(200));
    BEZEL_EXPECT_TRUE(r.ran);        // false pre-fix: 1592 B / 199 allocations
    BEZEL_EXPECT_EQ(r.exitCode, 7);  // every iteration's handler ran
}

// §27.5 / §28.9 / #4509 — the GUARDED cell: the captured local is declared
// OUTSIDE the loop, so `boxLocalSlot` allocates ONE cell and every iteration's
// closure shares it. N envs over one cell is the shape that makes a per-env
// free unsound and a refcount necessary: each iteration's env retains and its
// envdtor releases, and the frame's single release at the declaring scope's end
// is the one that reaches zero. Pre-fix: 12 B / 2 allocations — ONE shared 4 B
// cell for `k` plus the 8 B cell for `box`, not one per iteration, which is
// exactly what "N envs over one cell" predicts and why the row is about
// exactly-once reclamation rather than about volume.
BEZEL_TEST(escaping_capture_cell_refcount_4509,
           guarded_cell_shared_by_n_envs_reclaimed_once_spec_27_5) {
    auto r = runAotChecked(
        "module M; "
        "record Ev(N: int); "
        "class Box "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box; "
        "class Button "
        "  public event Clicked: Ev; "
        "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
        "end Button; "
        "class C public static method Main(): int "
        "  using var box := new Box(); "
        "  using var b := new Button(); "
        "  var k := 1; "
        "  for var i in 0..<50 do "
        "    do "
        "      using var sub := b.Clicked.Subscribe(lambda(e) => do box.Add(e.N + k); end); "
        "      b.Fire(1); "
        "    end; "
        "  end; "
        "  if box.V = 100 then return 9; end; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);        // false pre-fix: 12 B / 2 allocations
    BEZEL_EXPECT_EQ(r.exitCode, 9);  // 50 iterations x (1 + k) == 100
}

// §27.5 / §28.9 / §28.14 / #4509 — THE OVER-RELEASE PROBE, and the one row that
// would fail if the frame's release were a free. Here the env genuinely
// OUTLIVES the frame that created it: `Hook` subscribes through `this` (§28.14
// forms 3/4), storing the subscription in an `owned` field, so the handler runs
// after `Hook` has returned and reads the captured `k` THROUGH THE CELL.
//
// A static route cannot license a free at `Hook`'s scope end for this shape —
// which is precisely why #694 declines it — but the refcount can: the frame's
// release drops the count to one and the surviving env keeps the cell alive.
// `w.T == 5` is the load-bearing oracle: 2 (the raised value) + 3 (k, read from
// the cell after the frame is gone). Under `runAotChecked` a freed cell would
// fault at that read rather than quietly returning a wrong number, so the row
// catches the over-release two ways.
BEZEL_TEST(escaping_capture_cell_refcount_4509,
           env_outliving_the_frame_still_reads_the_cell_spec_28_14) {
    auto r = runAotChecked(
        "module M; "
        "record Ev(N: int); "
        "class Widget "
        "  private owned readonly _clicked: EventSource<Ev> := new(); "
        "  public property Clicked: IEvent<Ev> => _clicked; "
        "  public owned sub: Subscription?; "
        "  public T: int := 0; "
        "  public method Tally(x: int) T := T + x; end Tally; "
        "  public method Hook() "
        "    var k := 3; "
        "    this.sub := this.Clicked.Subscribe(lambda(e) => do this.Tally(e.N + k); end); "
        "  end Hook; "
        "  public method Fire(n: int) _clicked.Raise(new Ev(n)); end Fire; "
        "end Widget; "
        "class C public static method Main(): int "
        "  using var w := new Widget(); "
        "  w.Hook(); "
        "  w.Fire(2); "
        "  return w.T; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);        // false pre-fix: 4 B / 1 allocation
    BEZEL_EXPECT_EQ(r.exitCode, 5);  // 2 raised + 3 read through the surviving cell
}

// CONTROL (must be unchanged by the fix): a NON-escaping capture of the same
// `using var` class local. `cellFreeEligible_` admits it, so the pre-existing
// `boxedCellFreeDeferred` route frees the cell in the #1017 SECOND PASS and the
// refcount classification must NOT arm — the two routes are mutually exclusive
// and arming both would be the one way this change could double-free. Clean on
// both sides of the fix; the row exists to keep it that way.
BEZEL_TEST(escaping_capture_cell_refcount_4509,
           non_escaping_capture_keeps_the_scope_end_route_spec_27_5) {
    auto r = runAotChecked(
        "module M; "
        "class Box "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box; "
        "class C public static method Main(): int "
        "  var total := 0; "
        "  for var i in 0..<200 do "
        "    using var box := new Box(); "
        "    do "
        "      var g := lambda() => do box.Add(2); end; "
        "      g(); "
        "    end; "
        "    total := total + box.V; "
        "  end; "
        "  if total = 400 then return 7; end; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §27.5 / #4509 — VALUE CORRECTNESS, one shot, no leak oracle. The handler must
// still observe the captured object while the subscription is live, and must
// not be reached after it is disposed. Guards against the other way a cell fix
// can go wrong: reclaiming it too early and silently reading a zeroed cell.
BEZEL_TEST(escaping_capture_cell_refcount_4509,
           handler_still_observes_the_captured_local_spec_27_5) {
    auto r = runAot(
        "module M; "
        "record Ev(N: int); "
        "class Box "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box; "
        "class Button "
        "  public event Clicked: Ev; "
        "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
        "end Button; "
        "class C public static method Main(): int "
        "  using var box := new Box(); "
        "  using var b := new Button(); "
        "  do "
        "    using var sub := b.Clicked.Subscribe(lambda(e) => do box.Add(e.N); end); "
        "    b.Fire(2); "
        "  end; "
        "  b.Fire(5); "
        "  return box.V; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);  // the post-dispose Fire must not reach it
}
