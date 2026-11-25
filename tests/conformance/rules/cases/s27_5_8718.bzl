//! case: s27_5_8718
//! doc: spec
//! section: 27.5
//! title: an indexer read through a borrowed parameter with a lambda index argument that captures
//!   it
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5, 23.4, 28.14
//! expect: run
//! stdout: 2000
//! notes: Pins #9099 (borrowed receiver). `Use` indexes its plain (borrowed) parameter `f` with a
//!   lambda that captures `f` itself: q() + Tag = 10, 200 times: 2000.
module Conf;
public class Fl
  public Tag: int;
  public virtual property [q: () -> int]: int => q() + Tag;
  public constructor() Tag := 5; end constructor;
end Fl;
public class Program
  public static method Use(f: Fl): int
    var t := 0;
    for var j in 0 ..< 200 do
      t := t + f[lambda() => f.Tag];
    end for;
    return t;
  end Use;
  public static method Main(): int
    var fl := new Fl();
    Console.WriteLine($"{Program.Use(fl)}");
    return 0;
  end Main;
end Program;
