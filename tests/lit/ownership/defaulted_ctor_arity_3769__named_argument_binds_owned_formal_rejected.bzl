// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #3769: a NAMED argument filling the `owned` parameter while the
// defaulted one is omitted. §22.5: "Named arguments may appear in any order
// after positional arguments"; the owned-sink classification keys off the BOUND
// parameter, which it could not do while the formal list the binding resolves
// against did not exist. Accepted silently on the parent.

module M;

class Res
  private _id: int;
  public constructor(id: int) _id := id; end constructor;
  public method Id(): int => _id;
end Res;

class Base(owned r: Res, tag: int := 5)
  private owned _s: Res := r;
  private _t: int := tag;
  public method Id(): int
    return _t + _s.Id();
  end Id;
end Base;

class C
  public static method Main(): int
    var x := new Res(6);
    // CHECK: this call consumes `x`
    // CHECK-SAME: §22.3
    using var b := new Base(r := x);
    return b.Id();
  end Main;
end C;
