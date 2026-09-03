// §28.3 *Borrow lifetime rules* / §22.3 "Parameter modifiers" (v0.286), issue
// #3746 — the OUT-PARAMETER channel. An `out` parameter's location lives in
// the CALLER's frame, which outlives this call by construction, so it outlives
// an `owned` parameter that §28.3 *Auto-drop (affine)* disposes "at the end of
// the method": the written closure holds a borrow that dangles the moment the
// method returns.
//
// Measured on the parent (f517ddb): rc=0 with NO diagnostic, rc=139 (SIGSEGV)
// at run. The `using var` local twin is rejected with the sibling wording.
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
class C
  public static method Take(p: owned R, out g: () -> int)
    g := lambda(): int => p.V();
  end Take;
  public static method Main(): int
    Take(new R(), out var g);
    Console.WriteLine(g().ToString());
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: borrow of `p` written through an out/ref parameter would outlive its source
// CHECK-SAME: is an `owned` parameter, whose value THIS method disposes at its end
// CHECK-SAME: lives in the caller's frame
