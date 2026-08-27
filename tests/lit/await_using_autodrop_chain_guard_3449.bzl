// fix #3449 (codegen / disposal; language spec §28.12 "Asynchronous disposal",
// §28.9 "Runtime-managed types", §28.3 "Local variables, ownership, and
// borrows", v0.283): THE `await using` MIRROR of #2992's chain-guard oracle.
//
// tests/lit/using_var_autodrop_chain_guard_2992.bzl pins the same property for
// the PLAIN spelling. This file exists because that was the only spelling
// pinned, and the spelling was exactly what had gone wrong.
//
// ── WHAT WENT WRONG, AND WHY A PLAIN-SPELLING ORACLE COULD NOT SEE IT ──────
//
// `registerUsingDisposal`'s `if (ld->isAwaitUsing)` branch used to ALWAYS
// RETURN. Everything after it was dead code for that spelling: the ~25 residual
// per-built-in arms, and #2992's END-OF-CHAIN GUARD itself. So for `await
// using` the guard could not fire on ANY program — the failure mode was silence
// at the exact site whose whole job is to break silence, and 25 built-in heads
// took a word-0-as-vtable read (#2975's wild-jump shape) with nothing to say so.
//
// The guard's reachability is therefore the CENTRAL property of #3449's fix,
// and until this file it had no standing test at all. It was verified once, by
// hand, on two throwaway compilers — which protects nothing against the next
// refactor. That is the gap this file closes.
//
// ── WHAT THIS FILE PINS ────────────────────────────────────────────────────
//
// The same calibration its #2992 sibling pins, through the `await using`
// keyword: the guard must fire ONLY on an unclaimed built-in, never on a
// claimed one. Every built-in below is claimed by a dedicated arm that sits
// AFTER the `isAwaitUsing` branch — i.e. every one of them is an arm that was
// unreachable from this spelling before #3449 — so a correct compiler emits
// nothing. Three ways this goes red, and the third is the one that matters:
//
//   * an arm is REMOVED or stops matching -> its built-in falls off the end of
//     the chain, the guard fires, `NODIAG-NOT: internal:` fails;
//   * the guard's own predicate widens -> it fires on something claimed;
//   * ⚠ THE FALL-THROUGH IS RE-BROKEN -> if the `isAwaitUsing` branch returns
//     unconditionally again, every line below stops reaching its arm and the
//     `RUN` line's `-c` compile is no longer the whole story: the guard cannot
//     fire, so this file would go GREEN on a broken compiler. The `%bezelc -c`
//     ACCEPT control alone cannot catch that. What catches it is the
//     BEHAVIOURAL half in
//     tests/codegen/await_using_builtin_head_disposal_3449_test.cpp, whose 26
//     runAotChecked rows fail 18 ways under generated-code ASan/LSan. The two
//     files are complements and neither is sufficient: this one pins that the
//     guard stays CALIBRATED, that one pins that the arms stay REACHED.
//
// FAILS-WITHOUT, proven mechanically rather than asserted (the recipe its
// sibling records, run through this spelling): building bezelc with the
// Stopwatch arm disabled — `if (isSW)` -> `if (false && isSW)` in
// `registerUsingDisposal` (src/codegen/LlvmBackend_Setup.cpp) — makes THIS FILE
// FAIL on the `sw` line with
//
//   internal: `await using var` of disposable built-in `Stopwatch` has no
//   disposal arm — the value leaks at scope end (§28.3 / §28.9); this is a
//   compiler bug — #2992
//
// and restoring the arm makes it PASS. On the pre-#3449 compiler the SAME
// disabled arm produces NO note for this spelling at all (the guard is
// unreachable) while the plain-spelling sibling still reports one — which is
// precisely the asymmetry this file now forbids.
//
// The guard's POSITIVE direction is deliberately not pinned here, for the same
// reason as in the sibling: every built-in the chain can see is claimed, so a
// program that fired the guard would be a program proving the compiler is still
// broken.
//
// ⚠ SCOPE. The guard sees a declaration only when
// `Emitter::usingVarBuiltinHeadName` can NAME its head. A monomorphized generic
// head (`await using var s := new T();` at `T = Stopwatch`) is invisible to it
// and is NOT covered by this file — that shape still takes the word-0 read,
// identically before and after #3449, and is tracked as its own issue.
//
// Each local is USED, not merely bound: an unused `using var` risks the §28.10
// vacuous-pin lint, which is a Warning and would fail the NODIAG control for an
// unrelated reason. `await using` is valid only inside an async method or async
// lambda (§28.12), so the bindings live in an async body that `Main` drives.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// internal: / warning / #2992 / #3449 diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

class C
  public static async method Run(): Task<int>
    var acc: int := 0;

    // stdlib §6.9 — Channel<T>: the channel arm.
    await using var ch := new Channel<int>();
    acc := acc + ch.Count;                      // 0

    // stdlib §3.1 — StringBuilder: the #1185 arm.
    await using var sb := new StringBuilder();
    _ := sb.Append("x");
    acc := acc + sb.Length;                     // 1

    // stdlib §6.2 — Mutex, and the owned Lock its async acquisition mints.
    // `Acquire()` BLOCKS and is a compile error inside an async body, so the
    // Lock arrives through `AcquireAsync` — which also makes `lk` an INFERRED
    // head, exercising the initializer-type limb of the head resolution.
    await using var mu := new Mutex();
    await using var lk := await mu.AcquireAsync();

    // stdlib §2.7 — Random: the built-in #2921 found unclaimed for the plain
    // spelling. Its arm is the one the fails-without recipe above removes for
    // the sibling file; here Stopwatch plays that role.
    await using var rnd := new Random();
    acc := acc + rnd.Next(1);                   // 0..<1, i.e. always 0

    // stdlib §6.4 — Atomic<T> / AtomicInteger<T>: the #2973 pair.
    await using var at := new Atomic<int>(5);
    acc := acc + at.Value;                      // 5
    await using var ai := new AtomicInteger<int>(1);
    acc := acc + ai.Increment();                // 2

    // stdlib §8.1 — Stopwatch: the head #3449 was filed on, and the arm the
    // FAILS-WITHOUT recipe in this file's header disables.
    await using var sw := new Stopwatch();
    sw.Start();
    sw.Stop();

    // stdlib §6.5 — EventSource<T>: the #2026 arm.
    await using var es := new EventSource<int>();
    es.Raise(3);

    // §30.8 / stdlib §6.1 — CancellationTokenSource: the second head #3449 was
    // filed on (its fault was a null function pointer, `pc 0x0`).
    await using var cts := new CancellationTokenSource();
    cts.Cancel();

    return acc;                                 // 8
  end Run;

  public static method Main(): int
    return C.Run().Result;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: internal:
// NODIAG-NOT: no disposal arm
// NODIAG-NOT: #2992
