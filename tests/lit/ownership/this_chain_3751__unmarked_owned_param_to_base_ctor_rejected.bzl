// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #3751, the `base(...)` arm of the same defect (§12.8 "A derived class's
// constructor may chain to a base constructor"). Issue 3751 attributes the hole
// to the §22.10 primary having no ConstructorSymbol; measured on a31fb7b the
// base chain to an ORDINARY `constructor(owned r: Res)` was the same
// heap-use-after-free, because the missing lookup is on the chain CALLEE.

module M;
class Res
  private _id: int;
  public constructor(id: int) _id := id; end constructor;
  public method Id(): int => _id;
end Res;
class P
  private owned _s: Res;
  public constructor(owned r: Res)
    _s := move r;
  end constructor;
  public method Id(): int => _s.Id();
end P;
class D: P
  public constructor(owned x: Res)
    base(x);
  end constructor;
end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: this call consumes `x`; write `M(move x)` — passing the named owned binding `x` to an `owned` parameter transfers ownership and leaves it moved-from, which the call site must announce (§22.3
