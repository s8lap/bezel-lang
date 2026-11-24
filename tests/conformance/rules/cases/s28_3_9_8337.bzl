//! case: s28_3_9_8337
//! doc: spec
//! section: 28.3.9
//! title: a closure wrapped by a second closure that is called after the move of the owner is
//!   rejected
//! rule: A borrow is *live* at a consume site when some path from the site reaches a later use
//!   of it
//! cites: 28.3.9, 28.3
//! expect: reject
//! diagnostic: cannot consume
//! notes: Pins #8988 (the reject boundary through a wrapper): `h` captures `f`, which captures
//!   `a`; the later `h()` after the move is a use of a closure value that is itself still
//!   live. Declaring both in the body does not remove the straight-line later use.
module Conf;
import System;
public class Res
  public N: int := 7;
  public constructor() end constructor;
end Res;
public class Sink
  public static method Take(owned r: Res): int => 1;
end Sink;
public class Program
  public static method Main(): int
    var r := 0;
    for var i in 0 ..< 3 do
      var a := new Res();
      var f := lambda(): int => a.N;
      var h := lambda(): int => f();
      r := r + h();
      r := r + Sink.Take(move a);
      r := r + h();
    end;
    Console.WriteLine($"{r}");
    return 0;
  end Main;
end Program;
