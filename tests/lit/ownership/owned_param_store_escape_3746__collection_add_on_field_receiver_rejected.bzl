// stdlib-facing §28.11 "Borrowing collections" — *Storing a borrow* (v0.286),
// read with §28.3 *Auto-drop (affine)*; issue #3746.
//
// §28.11: "The store-site check is structural and decidable: parameter-,
// receiver-, and field-sourced borrows are admitted; a borrow of a
// **scope-bound local** stored into a collection that provably outlives that
// scope — the collection is a field, is reachable from one, escapes the
// method, or is a local of strictly wider scope — is **rejected** at the store
// site."
//
// The "parameter-sourced" admission carries §28.3's own premise for the same
// admission — the caller-side source outlives the store — and an `owned`
// parameter falsifies it: §28.3 disposes it "at the end of the method". So the
// element store of a borrow of one into a receiver reachable through `this`
// dangles the moment the method returns.
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
  public owned items: Array<R> := new Array<R>();
  public method Take(p: owned R)
    var b := p;
    this.items.Add(b);
  end Take;
end Box;
class C
  public static method Main(): int
    using var b := new Box();
    b.Take(new R());
    Console.WriteLine(b.items[0].V().ToString());
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: borrow of `p` passed to `.Add(...)` on `this` would outlive its source
// CHECK-SAME: is an `owned` parameter, whose value THIS method disposes at its end
// CHECK-SAME: §28.11 borrowing-collection store rule
