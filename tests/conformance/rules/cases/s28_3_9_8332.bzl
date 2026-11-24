//! case: s28_3_9_8332
//! doc: spec
//! section: 28.3.9
//! title: a closure aliased before the move of its owner and last used before it is admitted
//! rule: A borrow is *live* at a consume site when some path from the site reaches a later use
//!   of it
//! cites: 28.3.9, 28.3
//! expect: run
//! stdout: 400
//! notes: Pins #8988 for an alias `g` of the closure. Both `f` and `g` are declared in the body
//!   and the last use of `g` precedes the move; Per pass g() = 7 and Take = 1; 50 passes =
//!   400. Twin: s28_3_9_8336.
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
    for var i in 0 ..< 50 do
      var a := new Res();
      var f := lambda(): int => a.N;
      var g := f;
      r := r + g();
      r := r + Sink.Take(move a);
    end;
    Console.WriteLine($"{r}");
    return 0;
  end Main;
end Program;
