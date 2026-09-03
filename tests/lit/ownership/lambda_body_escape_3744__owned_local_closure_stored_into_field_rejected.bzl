// §28.3 *Borrow lifetime rules* clause 1 (v0.286), issue #3744: the FIELD
// destination of the owned-local escape, reached from inside a lambda body.
//
// §28.3 clause 1: "A borrowed scope-bound *local* never satisfies the rule —
// it is disposed at the end of its scope, which a field of the object outlives
// — so a borrowed local may not be assigned to any field."
// §28.9: the capture of an owned class-typed local "may not outlive that
// scope (§28.3)".
//
// fix #3727 restricted its enclosing-frame source retry to the plain-LOCAL
// target channel, so the `Dest::Field` leg still saw no source at all inside a
// lambda body. Measured on the parent (9feef15): rc=0, NO diagnostic, SIGSEGV
// at run. The METHOD-level twin (the same store hoisted out of `mk`) has been
// rejected for revisions.
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
    var total := 0;
    do
      using var r := new R();
      var mk := lambda(): int => do
        this.h := lambda(): int => r.V();
        return 0;
      end;
      total := mk();
    end;
    return total + this.h();
  end Run;
  public static method Main(): int
    using var c := new C();
    Console.WriteLine(c.Run().ToString());
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: borrow of `r` stored into a field would outlive its source
// CHECK-SAME: is an owned scope-bound local disposed at scope end
// CHECK-SAME: (§28.3's borrowed-field store rule
