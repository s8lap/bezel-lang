//! case: s28_3_9_8336
//! doc: spec
//! section: 28.3.9
//! title: an alias of a closure used after the move of its owner is rejected
//! rule: A borrow is *live* at a consume site when some path from the site reaches a later use
//!   of it
//! cites: 28.3.9, 28.3
//! expect: reject
//! diagnostic: cannot consume
//! notes: Pins #8988 (the reject boundary). Twin of s28_3_9_8332 with the use of the alias `g`
//!   moved after the move: the straight-line later use `g()` is reached from the consume
//!   site, and `g` holds the closure's borrow of `a`.
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
      var g := f;
      r := r + f();
      r := r + Sink.Take(move a);
      r := r + g();
    end;
    Console.WriteLine($"{r}");
    return 0;
  end Main;
end Program;
