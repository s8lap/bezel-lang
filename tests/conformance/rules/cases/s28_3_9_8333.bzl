//! case: s28_3_9_8333
//! doc: spec
//! section: 28.3.9
//! title: a const closure declared in a for body whose last use precedes the move of its owner
//!   is admitted
//! rule: A borrow is *live* at a consume site when some path from the site reaches a later use
//!   of it
//! cites: 28.3.9, 28.3
//! expect: run
//! stdout: 400
//! notes: Pins #8988 for a `const` closure local (§27.5: a `const` local declared in a loop
//!   body binds a fresh variable on every execution). Per pass f() = 7 and Take = 1; 50
//!   passes = 400.
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
      const f := lambda(): int => a.N;
      r := r + f();
      r := r + Sink.Take(move a);
    end;
    Console.WriteLine($"{r}");
    return 0;
  end Main;
end Program;
