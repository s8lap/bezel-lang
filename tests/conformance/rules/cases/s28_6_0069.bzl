//! case: s28_6_0069
//! doc: spec
//! section: 28.6
//! title: a method group over an owned field read inside its declaring class is admitted
//! rule: Explicit disposal ... requires a receiver the disposer **owns** ...: ...
//!   or an `owned` field ... read by code of the owning object's declaring type
//!   (§28.6, v0.343). Every receiver-side rule of the call the formation resolves
//!   as is read at the formation (§27.6, v0.343) — so forming `d.Dispose` over an
//!   owned field read inside the declaring class is admitted as the call
//!   `d.Dispose()` would be.
//! cites: 28.6, 27.6
//! expect: run
//! output: disposed
module Conf;

class D
  public constructor() end constructor;
  destructor Console.WriteLine("disposed"); end destructor;
end D;

public class Holder
  public owned d: D;
  public constructor() d := new D(); end constructor;
  public method Go()
    var f := d.Dispose;
  end Go;
end Holder;

public class Program
  public static method Main(): int
    using var h := new Holder();
    h.Go();
    return 0;
  end Main;
end Program;
