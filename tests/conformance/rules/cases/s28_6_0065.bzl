//! case: s28_6_0065
//! doc: spec
//! section: 28.6
//! title: a method group over an alias of a borrowed parameter's Dispose is rejected
//! rule: `Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: a borrowed local or parameter ...
//!   (§28.6, v0.343). Every receiver-side rule of the call the formation resolves
//!   as is read at the formation (§27.6, v0.343) — and an unmarked read of a
//!   class-typed binding is always a borrow (§28.3, v0.343) — so forming
//!   `b.Dispose` over an alias of a borrowed parameter is rejected as the call
//!   `b.Dispose()` would be.
//! cites: 28.6, 27.6, 28.3
//! expect: reject
//! diagnostic: cannot call `Dispose()` on `b`
module Conf;

class D
  public constructor() end constructor;
  destructor Console.WriteLine("disposed"); end destructor;
end D;

public class Program
  public static method Take(d: D): int
    var b := d;
    var f := b.Dispose;
    return 0;
  end Take;
end Program;
