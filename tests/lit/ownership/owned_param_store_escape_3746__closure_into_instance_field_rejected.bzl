// §28.3 "Local variables, ownership, and borrows" — *Captured owned bindings*,
// *Auto-drop (affine)*, and *Borrow lifetime rules* (v0.286); §28.9
// "Runtime-managed types"; issue #3746.
//
// §28.3 *Captured owned bindings*: "Inside a closure, a captured owned binding
// — an owned local or `owned` parameter of the enclosing body — is uniformly
// in **borrow state**".
// §28.3 *Auto-drop (affine)*: an undischarged owned binding "is **disposed
// implicitly at that scope end** (for an `owned` parameter, at the end of the
// method)".
// §28.3 *Borrow lifetime rules*: "A borrow may be stored in a field of an
// object only if the borrow's source provably outlives that object. … A borrow
// whose source *does* outlive the object **is** admissible; the idiomatic form
// binds it through a constructor parameter … where the caller-side source
// outlives the constructed object."
//
// THE HOLE: that admission's premise — the caller-side source outlives the
// object — is FALSE for an `owned` parameter, whose value this method disposes
// at its end. `ownedLocalBorrowSource` filtered its source to a `LocalSymbol`,
// so no source resolved and the store was accepted. Measured on the parent
// (f517ddb): rc=0 with NO diagnostic, rc=139 (SIGSEGV) at run. The
// byte-identical spelling over a method-scope `using var` local is rejected.
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
  public h: (() -> int)?;
  public method Take(p: owned R)
    this.h := lambda(): int => p.V();
  end Take;
end Box;
class C
  public static method Main(): int
    using var b := new Box();
    b.Take(new R());
    Console.WriteLine(b.h!().ToString());
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: borrow of `p` stored into a field would outlive its source
// CHECK-SAME: is an `owned` parameter, whose value THIS method disposes at its end
// CHECK-SAME: §28.3's borrowed-field store rule
