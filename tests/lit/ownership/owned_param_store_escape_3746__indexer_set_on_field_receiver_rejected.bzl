// stdlib-facing §28.11 "Borrowing collections" — *Storing a borrow* (v0.286),
// read with §28.3 *Auto-drop (affine)*; issue #3746 — the INDEXER-SET spelling
// of the element store. The compiler's own diagnostic says the two are one
// operation ("the indexer-set `coll[i] := value` is a store operation exactly
// like `.Add`"), but they are separate legs with separate source queries, so
// each earns a row.
//
// Measured on the parent (f517ddb): rc=0 with NO diagnostic; the program then
// prints a garbage int read out of freed memory. The `using var` local twin is
// rejected with the sibling wording.
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
  public owned items: Array<R?> := new Array<R?>();
  public method Take(p: owned R)
    this.items.Add(null);
    var b := p;
    this.items[0] := b;
  end Take;
end Box;
class C
  public static method Main(): int
    using var b := new Box();
    b.Take(new R());
    Console.WriteLine(b.items[0]!.V().ToString());
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: borrow of `p` stored into `this[...]` would outlive its source
// CHECK-SAME: is an `owned` parameter, whose value THIS method disposes at its end
