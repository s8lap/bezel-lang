// §28.3 *Borrow lifetime rules* (v0.286), issue #3744: the OUT/REF-PARAMETER
// destination of the owned-local escape, reached from inside a lambda body.
//
// The parameter's location is the CALLER's frame, which outlives this call, so
// the written closure dangles the moment the method returns — the same rule
// §28.3 states for the field destination, with a different holder. fix #3727's
// enclosing-frame retry was gated on the plain-local channel, so this leg saw
// no source at all inside a lambda body.
//
// Measured on the parent (9feef15): rc=0, NO diagnostic, SIGSEGV at run.
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
  public static method Fill(ref g: () -> int): int
    do
      using var r := new R();
      var mk := lambda(): int => do
        g := lambda(): int => r.V();
        return 0;
      end;
      var q := mk();
    end;
    return 0;
  end Fill;
  public static method Main(): int
    var g: () -> int := lambda(): int => 1;
    var z := Fill(ref g);
    Console.WriteLine((z + g()).ToString());
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: borrow of `r` written through an out/ref parameter would outlive its source
// CHECK-SAME: the parameter's location lives in the caller's frame
// CHECK-SAME: (§28.3 borrow-lifetime rule
