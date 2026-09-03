// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #3769 (§22.5 / §22.3 / §28.8, v0.286). A constructor call that OMITS a
// §22.5-defaulted trailing parameter matched no formal list by exact arity, so
// the §22.10 primary's `owned r` was invisible: `new Base(x)` compiled with NO
// diagnostic at all and was `AddressSanitizer: heap-use-after-free` at run time
// (the primary transferred `x` into `_s`, and the caller's `x` was freed again).
//
// §22.3: "Omitting the marker on a named owned binding is a compile error
// naming the fix". Adding the defaulted argument explicitly (`new Base(x, 5)`)
// already produced this diagnostic on the parent, which is what pinned the
// arity match as the whole defect.

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
    using var b := new Base(x);
    return b.Id();
  end Main;
end C;
