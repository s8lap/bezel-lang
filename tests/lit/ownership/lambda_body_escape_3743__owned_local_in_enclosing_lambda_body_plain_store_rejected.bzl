// §28.3 *Borrow lifetime rules* (v0.286), issue #3743: the PLAIN-STORE
// spelling of the enclosing-lambda-frame source — no closure at all.
//
// `checkBorrowStoreEscape` selects a different diagnostic for this shape (the
// closure sentence would name a closure the program does not contain), so the
// two spellings regress independently and each earns a row. §28.3: "A borrow
// may not be assigned to a local whose scope strictly encloses the borrow's
// source's scope ... the diagnostic is the standard *"Borrow held by `x` would
// outlive its source `y`."*"
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
  public static method Run(): int
    var holder: R? := null;
    var outer := lambda(): int => do
      using var r := new R();
      var mk := lambda(): int => do
        holder := r;
        return 0;
      end;
      return mk();
    end;
    return outer();
  end Run;
  public static method Main(): int
    Console.WriteLine(Run().ToString());
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: borrow held by `holder` would outlive its source `r`
// CHECK-SAME: (§28.3 borrow-lifetime rule
