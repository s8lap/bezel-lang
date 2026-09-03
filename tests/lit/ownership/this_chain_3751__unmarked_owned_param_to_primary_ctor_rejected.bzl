// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #3751 (§22.10 / §22.3 / §28.8, v0.286). A `this(...)` chain call's
// arguments were classified against NO formal list, because the chain callee is
// the bare identifier `this` and resolves to no ConstructorSymbol. The §22.10
// primary's `owned r: Res` was therefore invisible, so the convenience
// constructor's `owned` parameter `x` stayed Owned to the end of the body and
// was auto-dropped (§28.3) — freeing a value the primary had already
// transferred into `_s`. Measured: `AddressSanitizer: heap-use-after-free`.
//
// §22.3: "Omitting the marker on a named owned binding is a compile error
// naming the fix".

module M;
class Res
  private _id: int;
  public constructor(id: int) _id := id; end constructor;
  public method Id(): int => _id;
end Res;
class Base(owned r: Res)
  private owned _s: Res := r;
  public constructor(owned x: Res)
    this(x);
  end constructor;
  public method Id(): int
    return _s.Id();
  end Id;
end Base;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: this call consumes `x`; write `M(move x)` — passing the named owned binding `x` to an `owned` parameter transfers ownership and leaves it moved-from, which the call site must announce (§22.3
