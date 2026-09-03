// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #3779 (§22.5 / §22.3 / §28.8, v0.286). A constructor argument's
// owned-sink classification was keyed on its SOURCE position, not on the §22.5
// parameter it BINDS to. With `owned a` first and the borrow `b` second, a
// reordered named call put each argument at the other's source index.
//
// Measured on r12-cycle-1 @ 89eb914: compiles clean, `x` silently consumed.
// Measured on 5fbb8ab (#3769): REJECTED naming `y` — the argument bound to the
// BORROW formal — for which no correct spelling exists, `owned y` in a named
// argument being an A.7 MarkedTransfer grammar error.
//
// §22.5: "Named arguments may appear in any order after positional arguments";
// naming an argument "changes which parameter it binds to, never when it runs".

module M;

class Res
  private _id: int;
  public constructor(id: int) _id := id; end constructor;
  public method Id(): int => _id;
end Res;

class Two
  private owned _a: Res;
  private _bid: int;
  private _c: int;
  public constructor(owned a: Res, b: Res, c: int := 1)
    _a := move a;
    _bid := b.Id();
    _c := c;
  end constructor;
end Two;

class C
  public static method Main(): int
    var x := new Res(6);
    using var y := new Res(7);
    // CHECK-NOT: consumes `y`
    // CHECK-NOT: move out of `using var` `y`
    // CHECK: this call consumes `x`
    // CHECK-SAME: §22.3
    // CHECK-NOT: consumes `y`
    // CHECK-NOT: move out of `using var` `y`
    new Two(b := y, a := x);
    return 0;
  end Main;
end C;
