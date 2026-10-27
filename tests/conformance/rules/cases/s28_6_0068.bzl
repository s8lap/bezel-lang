//! case: s28_6_0068
//! doc: spec
//! section: 28.6
//! title: a method group over a using var owner's Dispose is admitted
//! rule: Explicit disposal ... requires a receiver the disposer **owns** ...: an
//!   owned local or `owned` parameter, a `using var`, an unbound owned temporary,
//!   a `move` result, or an `owned` field (§28.6, v0.343). Every receiver-side
//!   rule of the call the formation resolves as is read at the formation
//!   (§27.6, v0.343) — so forming `d.Dispose` over a `using var` owner is
//!   admitted as the call `d.Dispose()` would be.
//! cites: 28.6, 27.6
//! expect: run
//! output: disposed
module Conf;

class D
  public constructor() end constructor;
  destructor Console.WriteLine("disposed"); end destructor;
end D;

public class Program
  public static method Main(): int
    using var d := new D();
    var f := d.Dispose;
    return 0;
  end Main;
end Program;
