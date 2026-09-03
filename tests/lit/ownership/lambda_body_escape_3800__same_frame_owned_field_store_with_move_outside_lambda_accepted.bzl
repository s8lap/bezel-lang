// §28.3 *Borrow lifetime rules* / §28.9:7398 (v0.286), issue #3800 — the
// ACCEPT BOUNDARY of the E12 widening on the TRANSFER-LOCATION axis: the
// SAME-FRAME owned local is `move`d OUTSIDE the lambda body, and the field
// store happens in the same enclosing scope. This is the textbook §28.8
// transfer shape: a plain move into the field at method scope, where the
// dataflow tracks the move and the scope-end auto-drop on the local is
// suppressed.
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
  public owned a: R? := null;
end Box;
class C
  public method Run(): int
    var r := new R();
    var b := new Box();
    b.a := move r;     // textbook §28.8 transfer at method scope
    return b.a!.V();
  end Run;
  public static method Main(): int
    using var c := new C();
    Console.WriteLine(c.Run().ToString());
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
