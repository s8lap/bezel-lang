//! case: s27_5_8716
//! doc: spec
//! section: 27.5
//! title: a non-virtual indexer read with a lambda index argument that captures the receiver
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5, 23.4
//! expect: run
//! stdout: 2000
//! notes: Pins #9099 (non-virtual getter). `fl[lambda() => fl.Tag]` reads `q() + Tag` = 5 + 5 = 10,
//!   200 times: 2000.
module Conf;
public class Fl
  public Tag: int;
  public property [q: () -> int]: int => q() + Tag;
  public constructor() Tag := 5; end constructor;
end Fl;
public class Program
  public static method Main(): int
    var t := 0;
    var fl := new Fl();
    for var j in 0 ..< 200 do
      t := t + fl[lambda() => fl.Tag];
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
