// §28.3 *Auto-drop (affine)* / *Captured owned bindings* (v0.286), issue #3744
// (the parameter axis recorded on the issue): the FIELD destination with an
// `owned` PARAMETER as the source, from inside a lambda body.
//
// §28.3 *Auto-drop (affine)* disposes an undischarged `owned` parameter "at the
// end of the method", so a field of the target object outlives it exactly as it
// outlives an owned local. fix #3746 taught the `Dest::Field` leg to resolve an
// `owned` parameter — but only at METHOD scope: the enclosing-frame retry seeds
// itself from a map whose keys are declaration NODES, and `ParameterSymbol::decl`
// is null, so no parameter was ever in it.
//
// Measured on the parent (9feef15): rc=0, NO diagnostic, SIGSEGV at run —
// identically before and after fix #3746.
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
  public h: () -> int := lambda(): int => 0;
  public method Take(p: owned R): int
    var mk := lambda(): int => do
      this.h := lambda(): int => p.V();
      return 0;
    end;
    var t := mk();
    p.Dispose();
    return t;
  end Take;
end Box;
class C
  public static method Main(): int
    using var b := new Box();
    var r := new R();
    var t := b.Take(move r);
    Console.WriteLine((t + b.h()).ToString());
    return 0;
  end Main;
end C;

// The `owned`-parameter sentence fix #3746 added, now reached from a lambda body.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: borrow of `p` stored into a field would outlive its source
// CHECK-SAME: is an `owned` parameter, whose value THIS method disposes at its end
// CHECK-SAME: (§28.3's borrowed-field store rule
