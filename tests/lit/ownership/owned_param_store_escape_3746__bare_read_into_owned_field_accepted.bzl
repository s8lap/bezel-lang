// §28.8 "Ownership transfer" (v0.286); the second ACCEPT control for issue
// #3746 — the transfer pattern the pre-#3746 `LocalSymbol` filter was
// originally written to protect.
//
// A bare read of an `owned` parameter into an `owned` FIELD is a move-on-read
// into an owned slot (§28.3 producers; the `_resource := resource;` pattern of
// §28.8), not a borrow store: the field's object becomes the owner and nothing
// dangles. `checkBorrowStoreEscape`'s `targetIsOwnedField` skip runs BEFORE the
// borrow-source query, so widening that query cannot reach this program — this
// row is what proves the ordering held.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
import System;
class R
  private v: int := 5;
  public method V(): int
    return this.v;
  end V;
end R;
class Box
  public owned r: R?;
  public method Take(p: owned R)
    this.r := p;
  end Take;
end Box;
class C
  public static method Main(): int
    using var b := new Box();
    b.Take(new R());
    Console.WriteLine(b.r!.V().ToString());
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
