// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #3769: the `base(...)` arm of the same hole. A derived constructor
// chaining into a BASE class's §22.10 primary across an omitted §22.5 default
// resolved no formals, so §28.8's transfer never ran and the derived
// constructor's §28.3 auto-drop freed a value the base primary had already
// stored into `_s` — `AddressSanitizer: heap-use-after-free`, measured.

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
    return _s.Id();
  end Id;
end Base;

class D : Base
  public constructor(owned x: Res)
    // CHECK: this call consumes `x`
    // CHECK-SAME: §22.3
    base(x);
  end constructor;
end D;
