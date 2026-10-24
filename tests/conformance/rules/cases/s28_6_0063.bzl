//! case: s28_6_0063
//! doc: spec
//! section: 28.6
//! title: a method group over an owned field read outside its declaring class is rejected
//! rule: `Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: ... an owned-field read outside the
//!   declaring class (§28.6, v0.343). Every receiver-side rule of the call the
//!   formation resolves as is read at the formation (§27.6, v0.343) — so forming
//!   `h.d.Dispose` from outside the declaring class is rejected as the call
//!   `h.d.Dispose()` would be.
//! cites: 28.6, 27.6
//! expect: reject
//! diagnostic: cannot call `Dispose()` through a field read
module Conf;

class D
  public constructor() end constructor;
  destructor Console.WriteLine("disposed"); end destructor;
end D;

public class Holder
  public owned d: D;
  public constructor() d := new D(); end constructor;
end Holder;

public class Program
  public static method Main(): int
    using var h := new Holder();
    var f := h.d.Dispose;
    return 0;
  end Main;
end Program;
