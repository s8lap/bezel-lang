// §28.3 "Borrow lifetime rules" (v0.286), issue #3727 — the PLAIN-STORE
// spelling of the same channel: no closure at all, just `holder := r;` inside
// a lambda body, with `holder` declared at method scope and `r` an owned
// scope-bound local of a `do ... end` block the assignment sits inside.
//
// §28.3: "A borrow may not be assigned to a local whose scope strictly
// encloses the borrow's source's scope ... the diagnostic is the standard
// *\"Borrow held by `x` would outlive its source `y`.\"*"
//
// Kept as its own row because `checkBorrowStoreEscape` picks a DIFFERENT
// diagnostic for it (the closure sentence would name a closure the program
// does not contain), so the two spellings can regress independently — the
// same split fix #3520 made at method scope and #3565 mirrored for the catch
// binding. Both were equally unreachable from a lambda body before this fix.
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
    var total := 0;
    do
      using var r := new R();
      var mk := lambda(): int => do
        holder := r;
        return 0;
      end;
      total := mk();
    end;
    return total;
  end Run;
  public static method Main(): int
    Console.WriteLine(Run().ToString());
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: borrow held by `holder` would outlive its source `r`
// CHECK-SAME: (§28.3 borrow-lifetime rule
