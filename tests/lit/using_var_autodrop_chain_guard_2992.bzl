// fix #2992 (codegen / disposal; language spec §28.3 "Local variables,
// ownership, and borrows" and §28.9 "Runtime-managed types", v0.254): THE
// DIAGNOSTIC HALF of the `using var` auto-drop chain guard.
//
// #2992's fix added an end-of-chain guard to `registerUsingDisposal`
// (src/codegen/LlvmBackend_Setup.cpp): when a `using var` local's type is a
// vtable-less built-in CLASS that is not a value type, and no arm of the ~25-arm
// chain claimed it, the compiler now emits
//
//   internal: `using var` of disposable built-in `X` has no disposal arm —
//   the value leaks at scope end (§28.3 / §28.9); this is a compiler bug — #2992
//
// instead of emitting nothing at all in silence (the structural defect behind
// #2921 Random and #2973 Atomic/AtomicInteger).
//
// ── WHY THIS ORACLE IS HERE AND NOT IN GTEST (#3138) ──────────────────────
//
// The note above is a **Warning** on a program that COMPILES. `runAot` populates
// `JitResult::diagnostics` ONLY on a compile/link reject, a compiler crash or
// timeout, a fired sanitizer report, or a signal — and every one of those paths
// returns BEFORE `r.ran = WIFEXITED(status)` (tests/support/aot_support.hpp). So
// `EXPECT_TRUE(r.ran)` implies `r.diagnostics` is EMPTY, and the gtest form
//
//     BEZEL_EXPECT_TRUE(r.ran);
//     BEZEL_EXPECT_FALSE(hasChainGuardNote(r));   // scans r.diagnostics
//
// is a TAUTOLOGY: it cannot fail on any compiler, fixed or broken. That is
// exactly what the original #2992 regression file did, in all seven of its
// tests — the whole file's coverage was fictional, and its
// "HOW TO VERIFY FAILS-WITHOUT" recipe could not reproduce. This file replaces
// that half: lit runs the REAL `bezelc` and FileCheck sees its actual stderr, so
// a Warning on a compiling program IS observable here.
// (`scripts/check_vacuous_tests.py --strict` is the gate phase that keeps the
// vacuous shape from coming back.)
//
// The BEHAVIOURAL half — that each of these built-ins really is released at
// scope exit, measured under generated-code LeakSanitizer rather than inferred
// from the absence of a note — lives in
// tests/codegen/using_var_autodrop_chain_guard_2992_test.cpp.
//
// ── WHAT THIS FILE PINS ────────────────────────────────────────────────────
//
// The guard's CALIBRATION: it must fire ONLY on an unclaimed built-in, never on
// a claimed one. Every disposable built-in below is claimed by a dedicated arm,
// so a correct compiler emits nothing. Two ways this goes red:
//
//   * an arm is REMOVED or stops matching -> its built-in falls through the
//     chain, the end-of-chain guard fires, and `NODIAG-NOT: internal:` fails.
//     Proven mechanically: deleting `if (n == "Random") return "Random";` from
//     `Emitter::usingAutoDropVtablelessBuiltin`
//     (src/codegen/LlvmBackend_Disposal.cpp) makes this file FAIL with the
//     `internal: ... has no disposal arm` note on the `rnd` line, and restoring
//     it makes it PASS.
//   * the guard's own predicate widens (its value-type subtraction loses a name,
//     say) -> it fires on something it should not, and the same line fails.
//
// The guard's POSITIVE direction — that it DOES fire on an unclaimed built-in —
// is deliberately not pinned here: it is unreachable from source today, because
// every disposable built-in the chain can see is claimed by an arm. A test that
// could exercise it would be a test that the compiler still has the #2992 bug.
//
// Each local is USED, not merely bound: an unused `using var` risks the §28.10
// vacuous-pin lint, which is itself a Warning and would make the NODIAG control
// fail for an unrelated reason.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// internal: / warning / #2992 diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

import System.Diagnostics;

class C
  public static method Main(): int
    var acc: int := 0;

    // stdlib §6.9 — Channel<T>: claimed by the channel arm.
    using var ch := new Channel<int>();
    acc := acc + ch.Count;                      // 0

    // stdlib §3.1 — StringBuilder: claimed via the borrowing-Array<string>
    // free with the string-element release thunk (#1185).
    using var sb := new StringBuilder();
    _ := sb.Append("x");
    acc := acc + sb.Length;                     // 1

    // stdlib §6.2 — Mutex, and the owned Lock its Acquire mints.
    using var mu := new Mutex();
    using var g := mu.Acquire();

    // stdlib §2.7 — Random: the built-in #2921 found UNCLAIMED (it leaked
    // 3200 bytes in 200 allocations under LSan). Its arm is the one the
    // fails-without recipe above removes.
    using var rnd := new Random();
    acc := acc + rnd.Next(1);                   // 0..<1, i.e. always 0

    // stdlib §6.4 — Atomic<T> / AtomicInteger<T>: the pair #2973 found
    // unclaimed (4800 bytes in 200 allocations each).
    using var at := new Atomic<int>(5);
    acc := acc + at.Value;                      // 5
    using var ai := new AtomicInteger<int>(1);
    acc := acc + ai.Increment();                // 2

    // stdlib §8.1 — Stopwatch.
    using var sw := new Stopwatch();
    sw.Start();
    sw.Stop();

    // stdlib §6.5 — EventSource<T>: claimed via bz_event_source_free (#2026).
    using var es := new EventSource<int>();
    es.Raise(3);

    return acc;                                 // 8
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: internal:
// NODIAG-NOT: no disposal arm
// NODIAG-NOT: #2992
