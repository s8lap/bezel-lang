// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #3779, EXACT ARITY (§22.5 / §22.3 / §28.8, v0.286). A constructor argument's
// owned-sink classification was keyed on its SOURCE position, not on the §22.5
// parameter it BINDS to. With `owned a` first and the borrow `b` second, a
// reordered named call put each argument at the other's source index.
//
// The EXACT-ARITY twin: every parameter supplied, so no §22.5 default is
// filled and #3769's arity widening plays no part. Measured REJECTED naming
// `y` on BOTH 89eb914 and 5fbb8ab — the argument bound to the BORROW formal —
// for which no correct spelling exists, `owned y` in a named argument being an
// A.7 MarkedTransfer grammar error. This is the pre-existing half: #3769 did
// not introduce the defect, it extended its reach to defaulted-arity calls.
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
    new Two(b := y, a := x, c := 3);
    return 0;
  end Main;
end C;
