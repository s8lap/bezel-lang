// §28.3 *Borrow lifetime rules* / *Auto-drop (affine)* (v0.286), issue #3746 —
// the PLAIN-STORE spelling of the field channel: a bare read of an `owned`
// parameter into a NON-`owned` (borrow) field. §28.3's marker economy makes an
// unmarked read of a class-typed binding a borrow, never a consume ("An
// unmarked read of a class-typed binding never consumes"), so this stores a
// borrow of a value the method disposes at its end into storage that outlives
// the call.
//
// Into an `owned` field the same syntax is the §28.8 ownership TRANSFER and
// stays accepted — see the sibling accept row.
//
// Measured on the parent (f517ddb): rc=0 with NO diagnostic; the program then
// prints a garbage int read out of freed memory. The `using var` local twin
// has long been rejected with the sibling wording.
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
  public r: R?;
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

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: borrow of `p` stored into a field would outlive its source
// CHECK-SAME: is an `owned` parameter, whose value THIS method disposes at its end
