//! case: s17_13_0036
//! doc: spec
//! section: 17.13
//! title: discarding a using var disposes it in place, once, at the discard
//! rule: `_ := x;` where `x` is a named owned binding — an owned local, a `using
//!   var` that owns its value included (§28.6's receiver list: the discard
//!   disposes in place, as `x.Dispose()` does, and moves nothing out of the
//!   pin, axiom A5), or an `owned` parameter — is a marker-free consume: the
//!   value is disposed at the end of the discard statement, and `x` is
//!   thereafter treated exactly as explicitly disposed (the scope-end pass
//!   skips its disposal).
//! cites: 17.13, 28.6, 28.10
//! expect: run
//! output: disposed
//!   after discard
//!   end
//! issue: 7921
module Conf;

public class Res
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("disposed");
  end destructor;
end Res;

public class Program
  public static method Main()
    do
      using var c := new Res();
      _ := c;
      Console.WriteLine("after discard");
    end;
    Console.WriteLine("end");
  end Main;
end Program;
