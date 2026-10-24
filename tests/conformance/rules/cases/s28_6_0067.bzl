//! case: s28_6_0067
//! doc: spec
//! section: 28.6
//! title: a method group over an owned parameter's Dispose is admitted
//! rule: Explicit disposal ... requires a receiver the disposer **owns** ...: an
//!   owned local or `owned` parameter, a `using var`, an unbound owned temporary,
//!   a `move` result, or an `owned` field (§28.6, v0.343). Every receiver-side
//!   rule of the call the formation resolves as is read at the formation
//!   (§27.6, v0.343) — so forming `d.Dispose` over an `owned` parameter is
//!   admitted as the call `d.Dispose()` would be.
//! cites: 28.6, 27.6
//! expect: run
//! output: formed
//!   disposed
//!   0
module Conf;

class D
  public constructor() end constructor;
  destructor Console.WriteLine("disposed"); end destructor;
end D;

public class Program
  public static method Take(d: owned D): int
    var f := d.Dispose;
    Console.WriteLine("formed");
    return 0;
  end Take;
  public static method Main(): int
    Console.WriteLine(Take(new D()));
    return 0;
  end Main;
end Program;
