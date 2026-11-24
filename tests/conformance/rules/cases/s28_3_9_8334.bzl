//! case: s28_3_9_8334
//! doc: spec
//! section: 28.3.9
//! title: a closure whose owner is moved in the then arm that ends in break is admitted
//! rule: A borrow is *live* at a consume site when some path from the site reaches a later use
//!   of it
//! cites: 28.3.9, 28.3
//! expect: run
//! stdout: 32
//! notes: Pins #8988 with a break. After the first `f()` no use of `f` remains in the body; the
//!   then arm moves `a` and leaves the loop, the other path moves `a` after the last use.
//!   Passes i = 0..2 give 7 + 1 each (24); pass i = 3 gives 7 + 1 and breaks (8); total
//!   32.
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
    for var i in 0 ..< 6 do
      var a := new Res();
      var f := lambda(): int => a.N;
      r := r + f();
      if i = 3 then
        r := r + Sink.Take(move a);
        break;
      end;
      r := r + Sink.Take(move a);
    end;
    Console.WriteLine($"{r}");
    return 0;
  end Main;
end Program;
