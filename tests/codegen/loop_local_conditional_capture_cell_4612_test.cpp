// Codegen — issue #4612 (§27.5 Closures, spec v0.306): a lambda capturing a
// LOCAL DECLARED IN A LOOP BODY from inside a CONDITIONAL block left the cell
// holder null on every later iteration that did not re-enter the conditional,
// and the next read of the variable dereferenced it.
//
//     for var i in 0..<4 do
//       using var box := new Box();            /* declared IN the loop body */
//       using var b   := new Button();
//       if i mod 2 = 0 then
//         using var sub := b.Clicked.Subscribe(lambda(e) => box.Add(e.N));
//         b.Fire(10);                          /* cell allocated HERE only  */
//       end;
//       total := total + box.V;                /* odd i: loads a null holder */
//     end;
//
//     AddressSanitizer: SEGV on unknown address 0x000000000000
//     ... The signal is caused by a READ memory access.
//     ... Hint: address points to the zero page.
//
// WHY #3484 DID NOT ALREADY COVER IT. #3484 is the same failure mode over a
// captured PARAMETER (and over a local declared OUTSIDE the loop), and its
// repair — a null fall-back installed on every EXPOSED holder read by
// `fixupCapturedCellHolderReads` — reaches a read only when `boxedStorage`
// ENROLLED it, which it does only for a holder carrying a `cellVarSyncSlot_`
// entry. `boxLocalSlot` has two boxing strategies and only the GUARDED one
// recorded that entry. A local declared inside a loop body takes the OTHER,
// fresh-each-iteration strategy (#823's `loopBodyDeclaredSlots_` arm), so the
// entire fresh family sat outside the repair.
//
// WHY THE FRESH PATH NEEDS IT TOO, although it stores the holder
// unconditionally: that store is AT THE CAPTURE SITE, and every reclaim site
// NULLS the holder right after reclaiming — `boxedCellFree`, #1017's deferred
// free and #4509's `cellRelease` all do, deliberately, so that "boxLocalSlot's
// guard allocates a fresh cell on the next capture". On an iteration that does
// not reach the capture site again there is no next capture, and the null the
// previous iteration's reclaim wrote is what the read loads. The issue's own
// repro is the `cellRelease` spelling: an escaping lambda handed to
// `IEvent<T>.Subscribe` (stdlib §6.5), whose cell the declaring frame releases
// at loop-body scope end.
//
// Two halves to the fix, both required:
//   * the fresh path records its fall-back slot, so `boxedStorage` enrols the
//     read and #3484's repair covers it. Sound for the same reason it is on the
//     guarded path, and slightly more so here: on this family the DECLARATION
//     re-initialises that slot on every pass (it was lowered before the boxing,
//     so its store still targets the pre-boxing alloca), so the fall-back reads
//     THIS iteration's value. A for-var loop variable is excluded — its
//     original slot is the loop's own induction storage, managed by `incBB`.
//   * the guard's discriminator is `== null`, and the fresh path's holder is a
//     bare `entryAlloca`, so on an iteration BEFORE the first capture it holds
//     UNDEF and the discriminator cannot fire — measured as a SEGV reading the
//     holder back as 0x01f86bab63189069 (stack residue) on the `if i >= 2` row
//     below. `fixupCapturedCellHolderReads` therefore null-initialises the
//     holder itself, for exactly the holders it is guarding. Doing it at the
//     BOXING site instead — for every fresh-boxed local, guarded or not — was
//     tried and reverted: an entry-block `store null` clears a stack slot
//     LeakSanitizer's conservative scan had been treating as a live reference,
//     which turned `codegen_const_narrowing_capture_2597
//     .loop_declared_const_captures_per_iteration_spec_27_5` red over a
//     PRE-EXISTING leak (amplified 500x: 32000 B / 1500 allocations on
//     `cafdfc52`, 31936 B / 1497 with this fix — the same leak either way).
//     Keeping the store off the unguarded holders keeps every
//     unconditional-capture program bit-identical, the same discipline that
//     already keeps the guard off dominated reads (#3484).
//
// THE ORACLE IS THE VALUE, NOT "IT DID NOT CRASH". The pre-fix behaviour is a
// SEGV, so a `didNotCrash` / `failedAtCompileTimeWithRealLocation` row would
// pass vacuously on neither side and prove nothing about correctness. Every row
// asserts the exact number the program must print; a null-holder read cannot
// produce it, because the program dies at address 0 before the print.
//
// FAIL-WITHOUT / PASS-WITH, measured program-by-program on two compilers —
// `cafdfc52` (`origin/main`, which is also where the defect was CONFIRMED
// pre-existing: not introduced by anything in this batch) and this fix:
//
//   row                                          cafdfc52        this fix
//   loop-body LOCAL captured behind an `if`      SEGV @ 0        24
//   control: same program, NO `if`               44              44
//   capture skipped on the FIRST iteration       SEGV @ 0        24
//   loop-carried: per-iteration fresh binding    SEGV @ 0 (*)    1 10 21 30
//   control: same program, NO `if`               1 11 21 31      1 11 21 31
//   disposal is the ONLY reader of the local     SEGV @ 0        10
//
//   (*) printed "1" for iteration 0 and then died entering iteration 1 — the
//       shape whose crash is furthest from the capture site.
//
// The two controls are the issue's own discriminator: the identical program
// with the `if` removed (so the capture runs on every pass) is clean on BOTH
// compilers. That is what makes the conditional load-bearing rather than the
// loop, the escape, or the event.
//
// Every row is leak-clean under `bezelc --sanitize=address` with this fix, so
// nothing here is a `runAot` row standing in for an unfixed leak; `runAot` is
// used rather than `runAotChecked` because the gate's `--full` oracle already
// runs the whole suite under generated-code ASan.
//
// per §27.5 (issue #4612; siblings #3484, #2303, #4509, #823)

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string out(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// The event plumbing every row shares: a mutable `Box` for the captured local
// to point at, and a `Button` whose `Clicked` event gives the lambda somewhere
// to escape to (stdlib §6.5 — `Subscribe` returns an owned subscription, which
// is what makes the capture cell refcounted and reclaimed at scope end).
const char* kPrelude =
    "module M; import System; "
    "record Ev(N: int); "
    "class Box "
    "  public V: int := 0; "
    "  public method Add(x: int) V := V + x; end Add; "
    "end Box; "
    "class Button "
    "  public event Clicked: Ev; "
    "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
    "end Button; ";
}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// #4612 — the holder is null on the iteration that skipped the capture site
// ═══════════════════════════════════════════════════════════════════════════

// The issue's shape with a value attached. `box` is a `using var` declared in
// the `for` body, captured by an escaping lambda inside an `if` taken on half
// the iterations. `total := total + box.V` after the `if` is the exposed read.
// Per iteration `box.V` is 1 (the pre-capture `Add(1)`) plus 10 on a captured
// pass, so the total is 11 + 1 + 11 + 1 = 24.
// per §27.5 (issue #4612)
BEZEL_TEST(loop_local_conditional_capture_cell_4612,
           loop_body_local_captured_behind_if_spec_27_5) {
    BEZEL_EXPECT_EQ(out(
        std::string(kPrelude) +
        "class C public static method Main(): int "
        "  var total := 0; "
        "  for var i in 0..<4 do "
        "    using var box := new Box(); "
        "    using var b := new Button(); "
        "    box.Add(1); "
        "    if i mod 2 = 0 then "
        "      do "
        "        using var sub := b.Clicked.Subscribe(lambda(e) => do box.Add(e.N); end); "
        "        b.Fire(10); "
        "      end; "
        "    end; "
        "    total := total + box.V; "
        "  end; "
        "  Console.WriteLine(total.ToString()); "
        "  return 0; "
        "end Main; end C;"),
        "24\n");
}

// THE ISSUE'S OWN DISCRIMINATOR, and the row that keeps this file honest: the
// identical program with the `if` removed, so the capture runs on EVERY pass
// and the holder is re-stored before each read. Clean on both compilers — 11
// per iteration, 44 in total. If this row ever needed the fix, the diagnosis
// would be the loop or the escape, not the conditional.
// per §27.5 (issue #4612)
BEZEL_TEST(loop_local_conditional_capture_cell_4612,
           control_unconditional_capture_same_program_spec_27_5) {
    BEZEL_EXPECT_EQ(out(
        std::string(kPrelude) +
        "class C public static method Main(): int "
        "  var total := 0; "
        "  for var i in 0..<4 do "
        "    using var box := new Box(); "
        "    using var b := new Button(); "
        "    box.Add(1); "
        "    do "
        "      using var sub := b.Clicked.Subscribe(lambda(e) => do box.Add(e.N); end); "
        "      b.Fire(10); "
        "    end; "
        "    total := total + box.V; "
        "  end; "
        "  Console.WriteLine(total.ToString()); "
        "  return 0; "
        "end Main; end C;"),
        "44\n");
}

// The guard flipped so the FIRST iteration skips the capture. This is the other
// half of the fix: on iteration 0 no reclaim has run yet, so the holder is not
// a null written by `cellRelease` but the raw content of an uninitialised entry
// alloca — a fall-back keyed on `== null` is useless against it. Same total as
// the first row (two captured passes, two bare ones), reached the other way
// round.
// per §27.5 (issue #4612)
BEZEL_TEST(loop_local_conditional_capture_cell_4612,
           capture_skipped_on_first_iteration_spec_27_5) {
    BEZEL_EXPECT_EQ(out(
        std::string(kPrelude) +
        "class C public static method Main(): int "
        "  var total := 0; "
        "  for var i in 0..<4 do "
        "    using var box := new Box(); "
        "    using var b := new Button(); "
        "    box.Add(1); "
        "    if i >= 2 then "
        "      do "
        "        using var sub := b.Clicked.Subscribe(lambda(e) => do box.Add(e.N); end); "
        "        b.Fire(10); "
        "      end; "
        "    end; "
        "    total := total + box.V; "
        "  end; "
        "  Console.WriteLine(total.ToString()); "
        "  return 0; "
        "end Main; end C;"),
        "24\n");
}

// LOOP-CARRIED, over a VALUE-typed local so the cell genuinely holds the
// variable rather than a pointer to a shared object. Printing `n` per iteration
// pins both directions at once: the lambda's write must reach the frame's `n`
// on a captured pass (§27.5 "modifications inside the lambda are visible
// outside, and vice versa"), and a lambda formed in one iteration must NOT
// leave its cell behind for the next — `n` is re-initialised to `i * 10` every
// pass, so 1, 10, 21, 30 is the only sequence a fresh per-iteration binding can
// produce. A holder carried across the back edge would print the previous
// iteration's storage instead.
// per §27.5 (issue #4612)
BEZEL_TEST(loop_local_conditional_capture_cell_4612,
           loop_carried_fresh_binding_per_iteration_spec_27_5) {
    BEZEL_EXPECT_EQ(out(
        std::string(kPrelude) +
        "class C public static method Main(): int "
        "  for var i in 0..<4 do "
        "    var n := i * 10; "
        "    using var b := new Button(); "
        "    if i mod 2 = 0 then "
        "      do "
        "        using var sub := b.Clicked.Subscribe(lambda(e) => do n := n + e.N; end); "
        "        b.Fire(1); "
        "      end; "
        "    end; "
        "    Console.WriteLine(n.ToString()); "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        "1\n10\n21\n30\n");
}

// The control for the row above: same program, no `if`. Every pass captures, so
// every pass adds 1 — 1, 11, 21, 31 — and it is clean on both compilers.
// per §27.5 (issue #4612)
BEZEL_TEST(loop_local_conditional_capture_cell_4612,
           control_loop_carried_unconditional_capture_spec_27_5) {
    BEZEL_EXPECT_EQ(out(
        std::string(kPrelude) +
        "class C public static method Main(): int "
        "  for var i in 0..<4 do "
        "    var n := i * 10; "
        "    using var b := new Button(); "
        "    do "
        "      using var sub := b.Clicked.Subscribe(lambda(e) => do n := n + e.N; end); "
        "      b.Fire(1); "
        "    end; "
        "    Console.WriteLine(n.ToString()); "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        "1\n11\n21\n31\n");
}

// The issue's LITERAL shape: `box` has no read of its own after the `if` at
// all, so the only surviving reader of the holder is the `using var`'s own
// scope-end DISPOSAL — which goes through the same `boxedStorage` funnel and so
// dereferenced the same null. The printed value comes from an outer local the
// lambda also captures (a guarded-path capture, #3484's already-covered case),
// which keeps a real oracle on a program whose subject variable is write-only:
// two captured passes, `box.V` 0 → 5 on each, so 10.
// per §27.5 (issue #4612)
BEZEL_TEST(loop_local_conditional_capture_cell_4612,
           scope_end_disposal_is_the_only_reader_spec_27_5) {
    BEZEL_EXPECT_EQ(out(
        std::string(kPrelude) +
        "class C public static method Main(): int "
        "  var fired := 0; "
        "  for var i in 0..<4 do "
        "    using var box := new Box(); "
        "    using var b := new Button(); "
        "    if i mod 2 = 0 then "
        "      do "
        "        using var sub := b.Clicked.Subscribe(lambda(e) => do box.Add(e.N); fired := fired + box.V; end); "
        "        b.Fire(5); "
        "      end; "
        "    end; "
        "  end; "
        "  Console.WriteLine(fired.ToString()); "
        "  return 0; "
        "end Main; end C;"),
        "10\n");
}
