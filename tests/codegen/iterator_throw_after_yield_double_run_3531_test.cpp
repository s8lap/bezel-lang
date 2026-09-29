// Codegen — issue #3531 (spec §29.7 Disposal of partially-consumed iterators,
// §28.3 Local variables, ownership, and borrows rule 15 "Edge parity",
// §29.3 Iterator methods, v0.283): a SYNC ITERATOR body that THROWS after a
// `yield return` ran the disposals live at that suspend TWICE.
//
//     public static method Gen(v: int): IEnumerable<int>
//       using var r := new R();
//       yield return r.V;
//       if v > 2 then throw new InvalidOperationException("boom"); end if;
//       yield return r.V;
//     end Gen;
//
//     $ bezelc --sanitize=address -o p p.bzl && ./p
//     ==ERROR: AddressSanitizer: heap-use-after-free
//         #0 bz$iter$Gen$CloneDispose
//     freed by thread T0 here:
//         #1 bz$iter$Gen$MoveNext
//
// MoveNext's throw edge disposes `r` (§28.3 rule 15: "The exception edge
// performs the same implicit disposal as the normal edge, on the single
// establishment-ordered cleanup stack; the only difference is the lint"), then
// returns with the state field STILL PARKED at the last yield — so the
// consumer's `for`-in cleanup calls `Dispose`, the state switch matches that
// yield state, and it replays `smIterCleanupAtState_` over the object MoveNext
// just freed. Over a `string` local the same double-run is a SEGV inside the
// allocator (a double free through `bz_string_release`); over a manually driven
// enumerator a second `MoveNext` RESUMES PAST THE YIELD and reads the freed
// local itself.
//
// ── WHY IT IS NOT A NEW RULE. The remedy is #623's own rule, which this
// backend already states and already applies — but on one edge only. From
// `LlvmBackend_Stmts.cpp`'s `finUnwindBB` (the `try`/`finally` exception exit):
//
//     if we are inside an iterator state machine, mark the iterator as done
//     (state=-1) BEFORE running the finally body. … when the consumer's
//     for-loop cleanup subsequently calls Dispose, Dispose must see state=-1
//     and be a no-op — otherwise the finally would run a second time.
//
// That site is reached only when the throw is inside a `try`/`finally`. A body
// that throws with no enclosing `try` never passes through it, and nothing else
// stamped the state — so the identical replay happened, one disposal kind at a
// time, for every kind the snapshot carries. The fix moves the same stamp to
// `propagateBB()`, the single block on which an exception actually leaves
// MoveNext, and reuses `smStateGEPHere()` (#2891) as the "we are emitting inside
// this iterator's MoveNext right now" authority. No new predicate, no new state:
// -1 is already the compiler's encoding of "this activation is complete and its
// cleanups are discharged", which is what §29.7 requires of a body that threw —
// "Transitions to a completed state; subsequent `MoveNext()` returns `false`;
// second `Dispose()` is a no-op".
//
// ── DIRECTION. The throw edge OWNS the body's establishment-ordered stack
// (§28.3 edge parity) and `Dispose` stands down. That is the leak-safe
// direction: the alternative — have the throw edge skip and let `Dispose` own
// them — is unreachable in general, because a manually driven enumerator need
// never be disposed at all.
//
// ── MEASURED, this compiler vs. `b43196a` (its parent), `bezelc
// --sanitize=address`, LSan on:
//
//   row                                        b43196a              this commit
//   using var, throws after yield, x200        heap-use-after-free  1600, clean
//   string local, throws after yield, x200     SEGV in allocator    1200, clean
//   using var captured by a lambda, x200       heap-use-after-free  1600 / 0, clean
//   live for-in enumerator frame, x200         heap-use-after-free  1800, clean
//   manual enumerator, MoveNext after throw    heap-use-after-free  2000, clean
//   call-propagated throw, 3 using scopes      heap-use-after-free  700 / 0, clean
//   re-propagated past an inner catch, x100    heap-use-after-free  800 / 0, clean
//   CONTROL caught inside the iterator, x100   11400 / 0, clean     identical
//   PARITY abandon-by-break then exhaust       1400 / 4200, clean   identical
//   PARITY finally on the throw edge, x50      400 / 50, clean      identical
//   PARITY owning-iterator owned local         bal 1, leaks 16 B    identical
//   PARITY owned parameter, x100               800 / 0, clean       identical
//   PARITY async iterator twin, x50            400, clean           identical
//
// The five "identical" rows are the ones that say the stamp did not over-reach:
// three of them (abandon, finally, owned parameter) are paths on which some
// OTHER mechanism must still run the disposal, and they would show a leak or a
// wrong count if the new stamp had silenced it.
//
// ⚠ ASan IS THE ORACLE FOR EVERY THROWING ROW, and they must not be weakened to
// a plain value check. The pre-fix `using var` program printed the CORRECT total
// on a plain build; only ASan saw the use-after-free. That is #2633's two-oracle
// asymmetry in the form that costs the most — the failure these rows exist to
// catch is invisible to `runAot`.
// per §29.7, §28.3, §29.3 (issue #3531)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan with LeakSanitizer
// on, so a use-after-free / double-free / leak fails the row before the value
// is compared.
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
// Plain runAot for the one parity row whose shape still leaks by design.
std::string plainOut(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── The issue's repro (a): a `using var` object live across the yield the body
// throws after. Pre-fix: `heap-use-after-free … READ of size 8 … in
// bz$iter$Gen$CloneDispose`, freed in `bz$iter$Gen$MoveNext`. Amplified to 200
// enumerations so the row does not depend on one activation's allocator luck.
// Printed total: 200 x (7 for the first element + 1 for the catch) = 1600.
// per §29.7, §28.3, §29.3 (issue #3531)
BEZEL_TEST(iterator_throw_after_yield_double_run_3531,
           using_var_disposed_once_when_body_throws_after_yield_spec_29_7_28_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class R "
        "  public V: int := 7; "
        "end R; "
        "class C "
        "  public static method Gen(v: int): IEnumerable<int> "
        "    using var r := new R(); "
        "    yield return r.V; "
        "    if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "    yield return r.V; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      try "
        "        for var x in C.Gen(3) do acc := acc + x; end; "
        "      catch e: InvalidOperationException => "
        "        acc := acc + 1; "
        "      end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1600\n");
}

// ── The issue's repro (b): the SAME double-run over a `string` local (#2817's
// `Disposal{stringRelease}`), where replaying the release is a DOUBLE FREE
// rather than a use-after-free — pre-fix `SEGV on unknown address
// 0xfffffffffffffffe … in __asan::Allocator::Deallocate … bz_string_release …
// bz$iter$Gen$CloneDispose`. Worth its own row because the failure MODE differs:
// nothing here is a class instance and no vtable dispatch is involved, so a fix
// that only taught the object disposal to stand down would leave this crashing.
// Printed total: 200 x ("3-tag".Length = 5, + 1 for the catch) = 1200.
// per §29.7, §28.9, §28.3 (issue #3531)
BEZEL_TEST(iterator_throw_after_yield_double_run_3531,
           string_local_released_once_when_body_throws_after_yield_spec_29_7_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class C "
        "  public static method Gen(v: int): IEnumerable<int> "
        "    var s := v.ToString() + \"-tag\"; "
        "    yield return s.Length; "
        "    if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "    yield return s.Length; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      try "
        "        for var x in C.Gen(3) do acc := acc + x; end; "
        "      catch e: InvalidOperationException => "
        "        acc := acc + 1; "
        "      end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1200\n");
}

// ── The §27.5 shape #3500 had to steer around: a `using var` object CAPTURED by
// a lambda in the same body. `Cell.n` is a static instance counter, so the
// second printed line is a destructor-balance oracle independent of LSan — `0`
// means every one of the 200 `Cell`s this program constructs was disposed
// exactly once, neither twice (the defect) nor never (a fix that overshot).
// Pre-fix: `heap-use-after-free … in bz$iter$Gen$CloneDispose`.
// Printed totals: 200 x (7 + 1 for the catch) = 1600, then the balance.
// per §29.7, §27.5, §28.3 (issue #3531)
BEZEL_TEST(iterator_throw_after_yield_double_run_3531,
           captured_using_var_disposed_exactly_once_on_the_throw_edge_spec_29_7_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class Cell "
        "  public static n: int := 0; "
        "  public V: int := 0; "
        "  public constructor(v: int) Cell.n := Cell.n + 1; V := v; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Gen(v: int): IEnumerable<int> "
        "    using var c := new Cell(7); "
        "    var f := lambda(): int => c.V; "
        "    yield return f(); "
        "    if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "    yield return f(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      try "
        "        for var x in C.Gen(3) do acc := acc + x; end; "
        "      catch e: InvalidOperationException => "
        "        acc := acc + 1; "
        "      end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    Console.WriteLine(Cell.n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1600\n0\n");
}

// ── The THIRD entry kind the abandonment snapshot carries: §29.7 (#944)'s
// implicit for-in ENUMERATOR FRAME, live across the yield because the yield is
// inside the `for`-in body. Replaying it disposed an inner enumerator the throw
// edge had already disposed and freed — pre-fix `heap-use-after-free … in
// bz$iter$Gen$CloneDispose`, with `bz$iter$Inner$Dispose` on the free stack.
// Included because it is a different `IterCleanup::Kind` (EnumFrame, dispatched
// through the iterator vtable's slot 1) than the two rows above.
// Printed total: 200 x (1 + 7 for the first element + 1 for the catch) = 1800.
// per §29.7, §28.3, §29.3 (issue #3531)
BEZEL_TEST(iterator_throw_after_yield_double_run_3531,
           live_for_in_enumerator_frame_disposed_once_on_the_throw_edge_spec_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class R "
        "  public V: int := 7; "
        "end R; "
        "class C "
        "  public static method Inner(): IEnumerable<int> "
        "    yield return 1; yield return 2; yield return 3; "
        "  end Inner; "
        "  public static method Gen(v: int): IEnumerable<int> "
        "    using var r := new R(); "
        "    for var y in C.Inner() do "
        "      yield return y + r.V; "
        "      if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "    end; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      try "
        "        for var x in C.Gen(3) do acc := acc + x; end; "
        "      catch e: InvalidOperationException => "
        "        acc := acc + 1; "
        "      end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1800\n");
}

// ── THE HALF OF THE DEFECT THAT IS NOT ABOUT `Dispose` AT ALL, and the row that
// pins the §29.7 sentence directly: "Transitions to a completed state;
// subsequent `MoveNext()` returns `false`". A manually driven `IEnumerator<T>`
// iterator whose body threw was left parked at the last yield, so the NEXT
// `MoveNext()` RESUMED PAST THE YIELD — re-reading the `using var` its own
// throw edge had freed (pre-fix `heap-use-after-free … in bz$iter$Gen$MoveNext`,
// no `Dispose` on either stack) and reporting `true` for an element the body
// cannot produce. Post-fix the state is -1, the resume dispatch takes its
// exhausted default, and `MoveNext()` reports `false`.
// Printed total: 200 x (7 for the first element + 1 for the catch + 2 for
// `MoveNext` correctly reporting false) = 2000. The `1000` branch is the
// discriminator: a resume-past-the-yield scores 1000 per activation, so a
// regression here cannot be mistaken for an off-by-one.
// per §29.7, §29.3 (issue #3531)
BEZEL_TEST(iterator_throw_after_yield_double_run_3531,
           move_next_after_the_body_threw_reports_completed_spec_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class R "
        "  public V: int := 7; "
        "end R; "
        "class C "
        "  public static method Gen(v: int): IEnumerator<int> "
        "    using var r := new R(); "
        "    yield return r.V; "
        "    if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "    yield return r.V; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      using var en := C.Gen(3); "
        "      try "
        "        while en.MoveNext() do acc := acc + en.Current; end; "
        "      catch e: InvalidOperationException => "
        "        acc := acc + 1; "
        "      end; "
        "      if en.MoveNext() then acc := acc + 1000; else acc := acc + 2; end if; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "2000\n");
}

// ── PARITY row 1 — the path the fix must NOT touch. The same iterator, driven
// with NO throw: first abandoned by `break` after the first element (so
// `Dispose` IS the sole runner of the snapshotted cleanup stack, §29.7's
// "stops iterating early"), then run to exhaustion (so MoveNext's own scope exit
// is). Identical on both trees — 1400 then 1400 + 200 x 14 = 4200 — because the
// fix only ever stamps on `propagateBB`, which a body with no throw path never
// creates. This is also the control that a fix which stamped too eagerly would
// break: it would silence the abandonment replay and leak `r`, which LSan sees.
// per §29.7, §28.10 (issue #3531)
BEZEL_TEST(iterator_throw_after_yield_double_run_3531,
           parity_abandoned_and_exhausted_enumerations_unchanged_spec_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class R "
        "  public V: int := 7; "
        "end R; "
        "class C "
        "  public static method Gen(v: int): IEnumerable<int> "
        "    using var r := new R(); "
        "    yield return r.V; "
        "    if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "    yield return r.V; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      for var x in C.Gen(0) do acc := acc + x; break; end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    for var j in 0..<200 do "
        "      for var x in C.Gen(0) do acc := acc + x; end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1400\n4200\n");
}

// ── PARITY row 2 — #623's own edge, which already stamped -1 and must keep
// running its `finally` EXACTLY ONCE. `C.k` counts finally executions, so `50`
// for 50 activations is the assertion; `100` would mean the replay came back,
// `0` would mean the new stamp had displaced #623's. The `using var` inside the
// protected block makes the row sensitive to both edges at once.
// Printed totals: 50 x (7 for the first element + 1 for the catch) = 400, then
// the finally count.
// per §29.7, §31.2, §28.10 (issue #3531)
BEZEL_TEST(iterator_throw_after_yield_double_run_3531,
           parity_finally_on_the_throw_edge_still_runs_exactly_once_spec_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class R "
        "  public V: int := 7; "
        "end R; "
        "class C "
        "  public static k: int := 0; "
        "  public static method Gen(v: int): IEnumerable<int> "
        "    try "
        "      using var r := new R(); "
        "      yield return r.V; "
        "      if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "      yield return r.V; "
        "    finally "
        "      C.k := C.k + 1; "
        "    end; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<50 do "
        "      try "
        "        for var x in C.Gen(3) do acc := acc + x; end; "
        "      catch e: InvalidOperationException => "
        "        acc := acc + 1; "
        "      end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    Console.WriteLine(C.k.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "400\n50\n");
}

// ── PARITY row 3 — the one state-keyed disposal the -1 stamp could in principle
// have silenced: §29.7 (#2306)'s plain `owned` LOCAL of an `IOwningEnumerable`
// iterator, whose `Dispose` visit is gated on `establishedAtState <= state <
// transferredAtState` and therefore reads the parked state as a POSITION rather
// than as a sentinel. It is a `plainOut` row on purpose: this shape ALREADY
// leaks `Box(1)` (16 B / 1 allocation) on `b43196a`, because
// `collectYieldStatesForOwnedLocals` aborts its linear scan on any statement
// that is not a local declaration, a yield, a `try` or a `do` block — an `if`
// or a bare `throw` included — so `ownedLocalYieldStates` is empty and the
// state-keyed visit emits no IR at all for any body that can throw. The stamp
// therefore cannot regress it, and this row is the measurement that says so:
// identical output, identical leak, before and after. An LSan row here would
// enshrine a residue this commit does not touch.
// per §29.7, §29.8 (issue #3531)
BEZEL_TEST(iterator_throw_after_yield_double_run_3531,
           parity_owning_iterator_owned_local_state_keyed_visit_unchanged_spec_29_7) {
    BEZEL_EXPECT_EQ(plainOut(
        "module M; import System; import System.Collections.Generic; "
        "class Box "
        "  public static live: int := 0; "
        "  public static dead: int := 0; "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; Box.live := Box.live + 1; end constructor; "
        "  destructor Console.WriteLine(\"dispose \" + Id.ToString()); Box.dead := Box.dead + 1; end destructor; "
        "end Box; "
        "class C "
        "  public static method Gen(v: int): IOwningEnumerable<Box> "
        "    var a := new Box(1); "
        "    yield return new Box(2); "
        "    if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "    yield return a; "
        "  end Gen; "
        "  public static method Main(): int "
        "    Box.live := 0; Box.dead := 0; "
        "    try "
        "      using var source: IOwningEnumerable<Box> := C.Gen(3); "
        "      using var en: IOwningEnumerator<Box> := source.GetOwningEnumerator(); "
        "      while en.MoveNext() do "
        "        using var u := en.TakeCurrent(); "
        "      end; "
        "    catch e: InvalidOperationException => "
        "      Console.WriteLine(\"caught\"); "
        "    end; "
        "    Console.WriteLine(\"bal \" + (Box.live - Box.dead).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "dispose 2\ncaught\nbal 1\n");
}

// ── THE SHAPE THAT IS NOT A `throw` STATEMENT AT ALL, and the one that makes
// the blast radius concrete: the exception arrives from a CALL (`H.Boom`), the
// yield sits inside a `for` loop, and THREE `using var` scopes are live at that
// suspend — two outside the loop, one inside it. The call edge routes through
// `emitPendingCheck` rather than `emitThrowValue`, and both funnel into
// `unwindBB()`, so a fix placed at the `throw` statement would have missed this
// entirely. Pre-fix: `heap-use-after-free … in bz$iter$Gen$CloneDispose`.
//
// `Cell.n` is the destructor-balance oracle again, and here it carries more
// weight than in the rows above: `0` says all THREE cells of every one of the
// 100 activations were disposed exactly once, so the fix neither replayed a
// scope (the defect) nor dropped one (the over-correction). The two are
// distinguishable — a double run is an ASan report, a dropped scope is a
// positive `Cell.n` and an LSan report.
// Printed totals: 100 x (1 + 2 + 3 for the first element + 1 for the catch) =
// 700, then the balance.
// per §29.7, §28.3, §28.10 (issue #3531)
BEZEL_TEST(iterator_throw_after_yield_double_run_3531,
           call_propagated_throw_with_nested_using_scopes_disposes_once_spec_29_7_28_10) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class Cell "
        "  public static n: int := 0; "
        "  public V: int := 0; "
        "  public constructor(v: int) Cell.n := Cell.n + 1; V := v; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class H "
        "  public static method Boom(v: int): int "
        "    if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "    return v; "
        "  end Boom; "
        "end H; "
        "class C "
        "  public static method Gen(v: int): IEnumerable<int> "
        "    using var a := new Cell(1); "
        "    using var b := new Cell(2); "
        "    for var i in 0..<3 do "
        "      using var c := new Cell(3); "
        "      yield return a.V + b.V + c.V; "
        "      var z := H.Boom(v); "
        "      yield return z; "
        "    end; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<100 do "
        "      try "
        "        for var x in C.Gen(3) do acc := acc + x; end; "
        "      catch e: InvalidOperationException => "
        "        acc := acc + 1; "
        "      end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    Console.WriteLine(Cell.n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "700\n0\n");
}

// ── PARITY row 4 — an `owned` PARAMETER. ⚠ CORRECTED after review: this row
// does NOT reach §29.8 / #2154 / #2217's STATE-KEYED `disposeOwnedParams`
// predicate, as an earlier revision of this comment claimed. Its
// `yield return c.V;` is a member read, not a bare identifier, so the param
// scan aborts and the parameter takes the legacy monotone NULL-GUARD path
// instead. The conclusion it pins is still real and still worth pinning —
// `800` and a balance of `0`, identical on both trees, i.e. the parameter is
// disposed exactly once on the throw edge — but it is the null-guard path that
// is being pinned. The state-keyed positional predicate is covered by the a2 /
// a4 / c1 rows at the end of this file, which is where review found the
// regression this row could not see.
// Kept as a `checkedOut` row — unlike parity row 3, nothing here leaks on
// either tree, so the strict oracle is free.
// Printed totals: 100 x (7 for the first element + 1 for the catch) = 800,
// then the balance.
// per §29.8, §29.7 (issue #3531)
BEZEL_TEST(iterator_throw_after_yield_double_run_3531,
           parity_owned_parameter_disposed_once_on_the_throw_edge_spec_29_8) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class Cell "
        "  public static n: int := 0; "
        "  public V: int := 0; "
        "  public constructor(v: int) Cell.n := Cell.n + 1; V := v; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Gen(owned c: Cell, v: int): IEnumerable<int> "
        "    yield return c.V; "
        "    if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "    yield return c.V; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<100 do "
        "      try "
        "        for var x in C.Gen(new Cell(7), 3) do acc := acc + x; end; "
        "      catch e: InvalidOperationException => "
        "        acc := acc + 1; "
        "      end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    Console.WriteLine(Cell.n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "800\n0\n");
}

// ── PARITY row 5 — the ASYNC iterator twin, measured NOT to have this defect
// and pinned so it stays that way. `smKind_` scopes the new stamp to
// `SuspendKind::Yield`, so an async iterator's `Resume` is untouched; the same
// program shape over `IAsyncEnumerable<int>` is `400`, clean, on both trees,
// because the async path parks its state through its own completion protocol
// rather than leaving it at the last yield. This row is what turns "the async
// twin is fine" from a claim in a commit body into a measurement — and it is
// also the row that would catch the stamp leaking across the `smKind_` gate.
// Printed total: 50 x (7 for the first element + 1 for the catch) = 400.
// per §29.7, §30.7 (issue #3531)
BEZEL_TEST(iterator_throw_after_yield_double_run_3531,
           parity_async_iterator_throw_after_yield_unaffected_spec_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "import System.Threading.Tasks; "
        "class R "
        "  public V: int := 7; "
        "end R; "
        "class C "
        "  public static async method Gen(v: int): IAsyncEnumerable<int> "
        "    using var r := new R(); "
        "    yield return r.V; "
        "    if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "    yield return r.V; "
        "  end Gen; "
        "  public static async method Run(): Task<int> "
        "    var acc := 0; "
        "    for var j in 0..<50 do "
        "      try "
        "        await for var x in C.Gen(3) do acc := acc + x; end; "
        "      catch e: InvalidOperationException => "
        "        acc := acc + 1; "
        "      end; "
        "    end; "
        "    return acc; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := C.Run(); "
        "    t.Wait(); "
        "    Console.WriteLine(t.Result.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "400\n");
}

// ── THE OVER-REACH CONTROL, and the row that says the stamp fires on the right
// edge rather than on every exception. §29.6 forbids a `yield` inside a `try`
// body that has `catch` arms, but a `try`/`catch` with NO yield in it is legal
// in an iterator body — and an exception CAUGHT there must leave the state
// machine RUNNING: the catch dispatch is an `unwindTarget_`, not
// `propagateBB`, so the stamp must not fire and the body must go on to its next
// yield. A fix that marked the iterator done on any unwind edge would truncate
// this iterator to one element and leak nothing visible while doing it.
// Measured identical on both trees (this shape was never broken).
// Printed totals: 100 x (7 for the first element + 7 + 100 for the second) =
// 11400, then the balance. The 100 is the discriminator: it appears in the total
// only if the catch arm actually ran and the iterator resumed past it.
// per §29.6, §29.7, §31.2 (issue #3531)
BEZEL_TEST(iterator_throw_after_yield_double_run_3531,
           exception_caught_inside_the_iterator_does_not_complete_it_spec_29_6) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class Cell "
        "  public static n: int := 0; "
        "  public V: int := 0; "
        "  public constructor(v: int) Cell.n := Cell.n + 1; V := v; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class H "
        "  public static method Boom(v: int): int "
        "    if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "    return v; "
        "  end Boom; "
        "end H; "
        "class C "
        "  public static method Gen(v: int): IEnumerable<int> "
        "    using var r := new Cell(7); "
        "    yield return r.V; "
        "    var w := 0; "
        "    try "
        "      w := H.Boom(v); "
        "    catch e: InvalidOperationException => "
        "      w := 100; "
        "    end; "
        "    yield return r.V + w; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<100 do "
        "      for var x in C.Gen(3) do acc := acc + x; end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    Console.WriteLine(Cell.n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "11400\n0\n");
}

// ── The other half of that pair: the same in-iterator `try`/`catch`, but the
// arm names a type the thrown exception is NOT, so the catch dispatch
// RE-PROPAGATES outward and the exception does leave MoveNext. Pre-fix:
// `heap-use-after-free … in bz$iter$Gen$CloneDispose`. Together with the row
// above this pins both outcomes of the same `unwindTarget_` chain — handled
// (keep running) and unhandled (complete) — which is the distinction the stamp's
// placement at `propagateBB` rather than at the throw sites is there to make.
// Printed totals: 100 x (7 for the first element + 1 for the outer catch) = 800,
// then the balance.
// per §29.6, §29.7, §31.2 (issue #3531)
BEZEL_TEST(iterator_throw_after_yield_double_run_3531,
           exception_re_propagated_past_an_inner_catch_completes_the_iterator_spec_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class Cell "
        "  public static n: int := 0; "
        "  public V: int := 0; "
        "  public constructor(v: int) Cell.n := Cell.n + 1; V := v; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class H "
        "  public static method Boom(v: int): int "
        "    if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "    return v; "
        "  end Boom; "
        "end H; "
        "class C "
        "  public static method Gen(v: int): IEnumerable<int> "
        "    using var r := new Cell(7); "
        "    yield return r.V; "
        "    var w := 0; "
        "    try "
        "      w := H.Boom(v); "
        "    catch e: ArgumentException => "
        "      w := 100; "
        "    end; "
        "    yield return r.V + w; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<100 do "
        "      try "
        "        for var x in C.Gen(3) do acc := acc + x; end; "
        "      catch e: InvalidOperationException => "
        "        acc := acc + 1; "
        "      end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    Console.WriteLine(Cell.n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "800\n0\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// THE ROWS THAT PIN THE OTHER HALF OF THE STATE FIELD — added after review
// caught the first version of this fix REGRESSING them.
//
// `state` does double duty. It is a DISCHARGE record (what the -1/-2 sentinels
// and the `smIterCleanupAtState_` switch read) and it is a POSITION (what
// §29.8's "the state machine disposes any `owned` parameter that has not been
// moved out of by the body" — extended to plain owned locals by #2306 under
// §29.7 — reads as `established <= state < transferred`). On the throw edge the
// first is true and the second is NOT: the body threw where it stood. Stamping
// -1 for the first silently answered the second "past everything", and a local
// established before the throw and transferred after it was never disposed.
//
// THE PREMISE THAT WAS WRONG. An earlier revision of this commit argued the
// clash was unreachable because `collectYieldStatesForOwnedLocals` aborts its
// linear scan on any statement form it does not model, so `ownedLocalYieldStates`
// is empty for any body that can throw. It aborts on statement FORMS — but a
// `yield return <call>;` is a `YieldStmt`, which the scan TOLERATES: it is not a
// bare identifier, so the scan falls through and just advances the state. The
// throw comes from the call, i.e. from an EXPRESSION inside a tolerated
// statement. So the two coexist, and `disposeOwnedLocals` emits real IR
// (`grep -c '^itl\.'` = 4 on this shape, both compilers).
//
// The fix spills the position into a dedicated frame field on the throw edge
// before stamping the sentinel, and ONLY `disposeOwnedParams` /
// `disposeOwnedLocals` prefer the spill. Measured, x200:
//
//   row                          b43196a        first revision   as shipped
//   a2 manual enumerator         400/400 clean  400/200, 3200 B  400/400 clean
//   a4 idiomatic for-in          400/400 clean  400/200, 3200 B  400/400 clean
//   a3 same iterator, no throw   600/600 clean  identical        identical
//   c1 using var + owned local   heap-UAF       600/400, 3200 B  600/600 clean
//
// ⚠ Note what the b43196a column says: these shapes were CORRECT on the parent.
// They are here because nothing in the tree covered them — of 66 files
// mentioning `IOwningEnumerable`/`IOwningEnumerator`, only 3 also contain
// `throw new`, and in two of those the throw is in the CONSUMER. Parity row 3
// above misses it too, because its `if v > 2 then throw` is an `IfStmt`, which
// DOES abort the scan. A green gate would have vindicated the regression.
// per §29.8, §29.7, §28.3 (issue #3531)

// a2 — the plain owned local, driven through the manual `GetOwningEnumerator`
// protocol. `a` is established at state 1 and transferred at state 3; the body
// throws out of `C.Boom` while the state field reads 2, so `a` is still owed.
// `live`/`dead` are static construction/destruction counters, so `400 / 400` is
// a destructor-balance oracle independent of LSan: 200 activations x (Box(1) +
// Box(2)), each disposed exactly once.
BEZEL_TEST(iterator_throw_after_yield_double_run_3531,
           owned_local_transferred_at_a_later_yield_disposed_when_body_throws_spec_29_8) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class Box "
        "  public static live: int := 0; "
        "  public static dead: int := 0; "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; Box.live := Box.live + 1; end constructor; "
        "  destructor Box.dead := Box.dead + 1; end destructor; "
        "end Box; "
        "class C "
        "  public static method Boom(v: int): owned Box "
        "    if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "    return new Box(99); "
        "  end Boom; "
        "  public static method Gen(v: int): IOwningEnumerable<Box> "
        "    var a := new Box(1); "
        "    yield return new Box(2); "
        "    yield return C.Boom(v); "
        "    yield return a; "
        "  end Gen; "
        "  public static method Drive(v: int): int "
        "    do "
        "      using var source: IOwningEnumerable<Box> := C.Gen(v); "
        "      using var en: IOwningEnumerator<Box> := source.GetOwningEnumerator(); "
        "      try "
        "        while en.MoveNext() do "
        "          using var u := en.TakeCurrent(); "
        "        end; "
        "      catch e: InvalidOperationException => "
        "        return 1; "
        "      end; "
        "    end; "
        "    return 0; "
        "  end Drive; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    for var j in 0..<200 do caught := caught + C.Drive(3); end; "
        "    Console.WriteLine(\"caught=\" + caught.ToString()); "
        "    Console.WriteLine(\"live=\" + Box.live.ToString() + \" dead=\" + Box.dead.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "caught=200\nlive=400 dead=400\n");
}

// a4 — the same iterator through the IDIOMATIC `for var x in …` driver, which
// takes the per-loop CLONE path (`cloneDisposeFn`, `stateKeyedOwned` armed)
// rather than the source's own Dispose. Same numbers, different Dispose body:
// the positional read had to be corrected in both.
BEZEL_TEST(iterator_throw_after_yield_double_run_3531,
           owned_local_disposed_when_body_throws_under_the_for_in_driver_spec_29_8) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class Box "
        "  public static live: int := 0; "
        "  public static dead: int := 0; "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; Box.live := Box.live + 1; end constructor; "
        "  destructor Box.dead := Box.dead + 1; end destructor; "
        "end Box; "
        "class C "
        "  public static method Boom(v: int): owned Box "
        "    if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "    return new Box(99); "
        "  end Boom; "
        "  public static method Gen(v: int): IOwningEnumerable<Box> "
        "    var a := new Box(1); "
        "    yield return new Box(2); "
        "    yield return C.Boom(v); "
        "    yield return a; "
        "  end Gen; "
        "  public static method Main(): int "
        "    for var j in 0..<200 do "
        "      try "
        "        for var x in C.Gen(3) do using var u := owned x; end; "
        "      catch e: InvalidOperationException => Box.live := Box.live + 0; "
        "      end; "
        "    end; "
        "    Console.WriteLine(\"live=\" + Box.live.ToString() + \" dead=\" + Box.dead.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "live=400 dead=400\n");
}

// a3 — the CONTROL: the same iterator, same driver, `v = 1` so nothing throws
// and the body runs to exhaustion. Identical on all three compilers
// (600 / 600, clean). It is what says the spill did not perturb the ordinary
// exhaustion path, on which the parked field stays 0 and `positionalState`
// reduces to `disposeState` exactly.
BEZEL_TEST(iterator_throw_after_yield_double_run_3531,
           control_owning_iterator_without_a_throw_is_unchanged_spec_29_8) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class Box "
        "  public static live: int := 0; "
        "  public static dead: int := 0; "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; Box.live := Box.live + 1; end constructor; "
        "  destructor Box.dead := Box.dead + 1; end destructor; "
        "end Box; "
        "class C "
        "  public static method Boom(v: int): owned Box "
        "    if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "    return new Box(99); "
        "  end Boom; "
        "  public static method Gen(v: int): IOwningEnumerable<Box> "
        "    var a := new Box(1); "
        "    yield return new Box(2); "
        "    yield return C.Boom(v); "
        "    yield return a; "
        "  end Gen; "
        "  public static method Main(): int "
        "    for var j in 0..<200 do "
        "      for var x in C.Gen(1) do using var u := owned x; end; "
        "    end; "
        "    Console.WriteLine(\"live=\" + Box.live.ToString() + \" dead=\" + Box.dead.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "live=600 dead=600\n");
}

// ── THE ROW THAT HAD TO BE RIGHT ON BOTH HALVES AT ONCE, and the reason a
// simple "skip the stamp when `ownedLocalYieldStates` is non-empty" gate was
// NOT sufficient: the linear scan `continue`s past a `using var` declaration
// rather than aborting on it, so ONE body can carry both a registered
// state-keyed local AND a live `smIterCleanupAtState_` entry. Such a body needs
// the sentinel (or the `using var` is disposed twice) AND the parked position
// (or the owned local is never disposed).
//
//   b43196a          heap-use-after-free  (the #3531 double-run on `u`)
//   first revision   600 / 400, LSan 3200 B / 200  (double-run fixed, local lost)
//   as shipped       600 / 600, clean     (both, exactly once)
//
// 600 / 600 is 200 activations x (Box(7) + Box(1) + Box(2)).
// per §29.7, §29.8, §28.10, §28.3 (issue #3531)
BEZEL_TEST(iterator_throw_after_yield_double_run_3531,
           using_var_and_state_keyed_owned_local_each_disposed_once_spec_29_7_29_8) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class Box "
        "  public static live: int := 0; "
        "  public static dead: int := 0; "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; Box.live := Box.live + 1; end constructor; "
        "  destructor Box.dead := Box.dead + 1; end destructor; "
        "end Box; "
        "class C "
        "  public static method Boom(v: int): owned Box "
        "    if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "    return new Box(99); "
        "  end Boom; "
        "  public static method Gen(v: int): IOwningEnumerable<Box> "
        "    using var u := new Box(7); "
        "    var a := new Box(1); "
        "    yield return new Box(2); "
        "    yield return C.Boom(v); "
        "    yield return a; "
        "  end Gen; "
        "  public static method Drive(v: int): int "
        "    do "
        "      using var source: IOwningEnumerable<Box> := C.Gen(v); "
        "      using var en: IOwningEnumerator<Box> := source.GetOwningEnumerator(); "
        "      try "
        "        while en.MoveNext() do "
        "          using var t := en.TakeCurrent(); "
        "        end; "
        "      catch e: InvalidOperationException => "
        "        return 1; "
        "      end; "
        "    end; "
        "    return 0; "
        "  end Drive; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    for var j in 0..<200 do caught := caught + C.Drive(3); end; "
        "    Console.WriteLine(\"caught=\" + caught.ToString()); "
        "    Console.WriteLine(\"live=\" + Box.live.ToString() + \" dead=\" + Box.dead.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "caught=200\nlive=600 dead=600\n");
}
