// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #3751, second face. A plain (BORROW) parameter chained into an `owned`
// primary-constructor slot: the borrow's real owner disposes the value and the
// constructed object's generated `Dispose()` disposes the `owned` field it was
// stored in. Measured on a31fb7b this compiled with NO diagnostic at all and
// faulted with `AddressSanitizer: heap-use-after-free`, because §28.8's
// borrow-into-`owned`-formal check also needs the chain target's formals.

module M;
class Res
  private _id: int;
  public constructor(id: int) _id := id; end constructor;
  public method Id(): int => _id;
end Res;
class Base(owned r: Res)
  private owned _s: Res := r;
  public constructor(b: Res)
    this(b);
  end constructor;
  public method Id(): int
    return _s.Id();
  end Id;
end Base;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow cannot be passed where an `owned` value is required: an `owned` sink takes responsibility for disposing the value, but a borrow is owned elsewhere (§28.8
