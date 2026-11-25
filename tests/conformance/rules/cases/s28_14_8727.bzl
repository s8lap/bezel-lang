//! case: s28_14_8727
//! doc: spec
//! section: 28.14
//! title: a closure local passed as an index argument to a generic class indexer
//! rule: the callee receives the callback for the duration of the call only
//! cites: 28.14, 23.4, 14.1
//! expect: run
//! stdout: 1400
//! notes: Pins #9102 (generic receiver). `FlG<int>`'s indexer returns `q()`: 7 per read, 200 reads:
//!   1400.
module Conf;
public class FlG<T>
  public property [q: () -> int]: int => q();
  public constructor() end constructor;
end FlG;
public class Program
  public static method Main(): int
    var t := 0;
    for var j in 0 ..< 200 do
      using var fl := new FlG<int>();
      var g := lambda() => 7;
      t := t + fl[g];
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
