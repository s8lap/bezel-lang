//! case: s28_6_0061
//! doc: spec
//! section: 28.6
//! title: a method group over a borrowed parameter's Dispose is rejected
//! rule: `Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: a borrowed local or parameter ...
//!   (§28.6, v0.343). Every receiver-side rule of the call the formation resolves
//!   as is read at the formation (§27.6, v0.343) — so forming `d.Dispose` over a
//!   non-`owned` parameter is rejected as the call `d.Dispose()` would be.
//! cites: 28.6, 27.6
//! expect: reject
//! diagnostic: cannot call `Dispose()` on parameter `d`
module Conf;

class D
  public constructor() end constructor;
  destructor Console.WriteLine("disposed"); end destructor;
end D;

public class Program
  public static method Take(d: D): int
    var f := d.Dispose;
    return 0;
  end Take;
  public static method Main(): int
    using var d := new D();
    Console.WriteLine(Take(d));
    return 0;
  end Main;
end Program;
