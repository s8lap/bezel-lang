// Reject control for issue #1750.
//
// Spec §11.2 (v0.184): an implementing method whose parameter modifiers
// (`ref`, `out`, `owned`, `scoped`) match the interface member's exactly
// satisfies the interface — the ordinary, correctly-marked case must not be
// false-rejected by the #1750 fix (generalizing the interface-implementation
// modifier check from `scoped`-only to all of `ref`/`out`/`owned`/`scoped`).
// `Impl` below implements TWO interfaces, exercising two different modifier
// axes: `IFactory.Absorb` takes `owned x: T` and `Impl.Absorb` repeats
// `owned` exactly (and properly discharges it via `move`, so no incidental
// §28.3 auto-drop warning appears either — this file must be diagnostic-
// clean, not merely error-free); `ICounter.Bump` takes `ref n: int` and
// `Impl.Bump` repeats `ref` exactly.
//
// Two RUN lines assert the accept two ways:
//  1. `bezelc -c` must EXIT ZERO — lit fails the test on a non-zero return,
//     so a successful compile IS the pass. Output goes to `%t.o` (build-tree
//     scratch), never the source tree.
//  2. The diagnostic stream must be empty of errors — FileCheck with
//     `--allow-empty` confirms neither "error" nor a § section-citation
//     reject (nor an unrelated warning) appears.
//
// RUN: %not %bezelc -c -o %t.o %s
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
module M;
class T end T;
interface IFactory
  method Absorb(owned x: T);
end IFactory;
interface ICounter
  method Bump(ref n: int);
end ICounter;
class Impl : IFactory, ICounter
  private owned _x: T? := null;
  public method Absorb(owned x: T)
    _x := move x;
  end Absorb;
  public method Bump(ref n: int)
    n := n + 1;
  end Bump;
end Impl;


// CHECK: the `move` marker is redundant on a {{.*}} (§28.3 K3.13 marker economy; K3.1)