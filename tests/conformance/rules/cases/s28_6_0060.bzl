//! case: s28_6_0060
//! doc: spec
//! section: 28.6
//! title: a method group over `this.Dispose` is rejected at the formation
//! rule: `Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: a borrowed local or parameter — including
//!   `this`: an object may not dispose itself out from under its owner, ...
//!   (§28.6, v0.343). Every receiver-side rule of the call the formation resolves
//!   as is read at the formation (§27.6, v0.343) — so forming `this.Dispose` is
//!   rejected as the call `this.Dispose()` would be.
//! cites: 28.6, 27.6
//! expect: reject
//! diagnostic: cannot call `Dispose()` on `this`
module Conf;

class D
  public constructor() end constructor;
  destructor Console.WriteLine("disposed"); end destructor;
end D;

public class Holder
  public owned d: D;
  public constructor() d := new D(); end constructor;
  public method Go(): int
    var f := this.Dispose;
    return 0;
  end Go;
end Holder;

public class Program
  public static method Main(): int
    using var h := new Holder();
    Console.WriteLine(h.Go());
    return 0;
  end Main;
end Program;
