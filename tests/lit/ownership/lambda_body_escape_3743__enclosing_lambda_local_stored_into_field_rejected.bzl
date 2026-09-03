// §28.3 / §28.9 (v0.286), issues #3743 + #3744 COMPOSED: the source is
// declared in an ENCLOSING LAMBDA's body (#3743's axis) and the destination is
// a FIELD (#3744's axis). Neither fix alone reaches it — one supplies the
// frame the source lives in, the other opens the channel the store travels on
// — so this row is the one that fails if either half is reverted.
//
// Measured on the parent (9feef15): rc=0, no diagnostic.
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
  private h: () -> int := lambda(): int => 0;
  public method Run(): int
    var outer := lambda(): int => do
      using var r := new R();
      var mk := lambda(): int => do
        this.h := lambda(): int => r.V();
        return 0;
      end;
      return mk();
    end;
    return outer() + this.h();
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
