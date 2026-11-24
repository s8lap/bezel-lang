//! case: s28_3_9_8331
//! doc: spec
//! section: 28.3.9
//! title: a closure declared in a while body whose last use precedes the move of its owner is
//!   admitted
//! rule: A borrow is *live* at a consume site when some path from the site reaches a later use
//!   of it
//! cites: 28.3.9, 28.3
//! expect: run
//! stdout: 1600
//! notes: Pins #8988 (the issue's shape in a `while`). `f` and `a` are both declared in the
//!   body, so each pass has a fresh pair; the last use of `f` precedes the move of `a`,
//!   and the next pass uses a different `f`, so no later use of the borrow follows the
//!   consume site. Per pass f() = 7 and Take = 1; 200 passes = 1600. Twin: s28_3_9_8336.
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
    var j := 0;
    while j < 200 do
      var a := new Res();
      var f := lambda(): int => a.N;
      r := r + f();
      r := r + Sink.Take(move a);
      j := j + 1;
    end;
    Console.WriteLine($"{r}");
    return 0;
  end Main;
end Program;
