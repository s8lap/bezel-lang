//! case: s28_6_0062
//! doc: spec
//! section: 28.6
//! title: a method group over a non-owned field read's Dispose is rejected
//! rule: `Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: ... a non-`owned` field read (§28.6,
//!   v0.343). Every receiver-side rule of the call the formation resolves as is
//!   read at the formation (§27.6, v0.343) — so forming `this._d.Dispose` is
//!   rejected as the call `this._d.Dispose()` would be.
//! cites: 28.6, 27.6
//! expect: reject
//! diagnostic: cannot call `Dispose()` through a field read
module Conf;

class D
  public constructor() end constructor;
  destructor Console.WriteLine("disposed"); end destructor;
end D;

public class Holder
  private _d: D;
  public constructor(d: D) _d := d; end constructor;
  public method Go(): int
    var f := this._d.Dispose;
    return 0;
  end Go;
end Holder;

public class Program
  public static method Main(): int
    using var d := new D();
    using var h := new Holder(d);
    Console.WriteLine(h.Go());
    return 0;
  end Main;
end Program;
