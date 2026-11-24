//! case: s28_3_9_8335
//! doc: spec
//! section: 28.3.9
//! title: a closure over an owner moved inside a try whose finally does not use the closure is
//!   admitted
//! rule: A borrow is *live* at a consume site when some path from the site reaches a later use
//!   of it
//! cites: 28.3.9, 28.3
//! expect: run
//! stdout: 450
//! notes: Pins #8988 with a try/finally. The finally block adds 1 and never uses `f`, so no
//!   later use of the borrow follows the move on any path. Per pass 7 + 1 + 1 = 9; 50
//!   passes = 450. Twin: s28_3_9_8338.
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
      try
        r := r + f();
        r := r + Sink.Take(move a);
      finally
        r := r + 1;
      end;
    end;
    Console.WriteLine($"{r}");
    return 0;
  end Main;
end Program;
