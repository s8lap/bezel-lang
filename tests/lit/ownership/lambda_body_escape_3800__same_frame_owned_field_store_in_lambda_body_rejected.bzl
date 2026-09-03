// §28.3 *Borrow lifetime rules* clause 1 / §28.9:7398 / §28.3 *Captured owned
// bindings* (v0.286), issue #3800 — the SAME-FRAME twin of E11 (#3744):
// `b.a := r;` over `var r := new R();` and `var b := new Box();` BOTH declared
// in the lambda body's OWN frame (no enclosing-frame source).
//
// §28.3 clause 1: "A borrowed scope-bound *local* never satisfies the rule —
// it is disposed at the end of its scope, which a field of the object outlives
// — so a borrowed local may not be assigned to any field."
//
// The E11 widening (#3744) reached the rejection by NOT finding the captured
// binding in the body's re-seeded `states_` map and retrying against the
// enclosing frame; this widening reaches it WITHOUT the retry, because the
// body's own seed holds the SAME-FRAME local. Measured on the parent (3729220,
// `--sanitize=address`): rc=0, NO diagnostic, dies with
// `heap-use-after-free … READ of size 8 … in bz$M.Box$DisposeCore` — the
// lambda body's scope-end auto-drop disposes `r` while the field now also owns
// it. The #402 `targetIsOwnedField` move-on-read skip fired on the body's own
// `lookupState(name)` result and the dataflow's `markMovedByName` was reached
// but never consumed by the lambda body's scope-end auto-drop.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;
import System;
class R
  private v: int := 5;
  public method V(): int
    return this.v;
  end V;
end R;
class Box
  public owned a: R? := null;
end Box;
class C
  public method Run(): int
    var mk := lambda(): int => do
      var r := new R();
      var b := new Box();
      b.a := r;     // E12: same-frame owned-field store, UAF in Box$DisposeCore
      return b.a!.V();
    end;
    return mk();
  end Run;
  public static method Main(): int
    using var c := new C();
    Console.WriteLine(c.Run().ToString());
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: borrow of `r` stored into a field would outlive its source
// CHECK-SAME: is an owned scope-bound local disposed at scope end
// CHECK-SAME: From inside a closure the value cannot reach the field
// CHECK-SAME: §28.3 *Captured owned bindings* / §28.9:7398
