// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// The DIAGNOSTIC-SILENCE oracle for fix #4747 (stdlib §4.19 v0.307:
// `FrozenArray<T>.ToArray(): owned Array<T>` — "fresh mutable copy back out").
//
// Measured pre-fix with the real `build/bezelc`: an UNANNOTATED local bound to
// `fa.ToArray()` was never typed by sema, so the local degraded to a
// zero-initialized slot and every later read of it emitted nothing at all —
//
//   warning: internal: the argument of this `Console.WriteLine` failed to
//   lower to a value, so the whole statement emits nothing — not even the
//   line terminator (stdlib §2.1 / §21.9) — #3079
//
// ...with the program still compiling and exiting 0. The gtest sibling
// (tests/codegen/frozen_array_toarray_typing_4747_test.cpp) owns the VALUES on
// stdout; this file owns the SILENCE, which `runAot` cannot see — it records
// diagnostics only on a compile REJECT, and this program compiles.
//
// NODIAG-NOT: internal:
// NODIAG-NOT: #3079
// NODIAG-NOT: #2434

module FA4747;

class FA
  public static method Main(): int
    var buf := new OwningBuffer<int>(2, lambda(i: int): int => i);
    using var fa := FrozenArray<int>.From(move buf);

    // The issue's repro: no annotation on the receiving local.
    using var back := fa.ToArray();
    back[0] := 99;
    Console.WriteLine(back[0]);
    Console.WriteLine(fa[0]);

    // The reduced form named in the issue — a bare `.Count` read was enough
    // to lose the whole statement pre-fix.
    using var back2 := fa.ToArray();
    Console.WriteLine(back2.Count);

    return 0;
  end Main;
end FA;
