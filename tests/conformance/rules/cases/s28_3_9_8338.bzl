//! case: s28_3_9_8338
//! doc: spec
//! section: 28.3.9
//! title: a closure used in the finally of a try that moves its owner is rejected
//! rule: A borrow is *live* at a consume site when some path from the site reaches a later use
//!   of it
//! cites: 28.3.9, 28.3
//! expect: reject
//! diagnostic: cannot consume
//! notes: Pins #8988 (the reject boundary through finally). Twin of s28_3_9_8335 with `f()` in
//!   the finally: the finally block runs after the move on the normal path, a later use
//!   reached from the consume site.
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
      try
        r := r + f();
        r := r + Sink.Take(move a);
      finally
        r := r + f();
      end;
    end;
    Console.WriteLine($"{r}");
    return 0;
  end Main;
end Program;
