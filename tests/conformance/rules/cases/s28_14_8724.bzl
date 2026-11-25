//! case: s28_14_8724
//! doc: spec
//! section: 28.14
//! title: one closure local used by a sum of two index reads
//! rule: the callee receives the callback for the duration of the call only
//! cites: 28.14, 23.4
//! expect: run
//! stdout: 2800
//! notes: Pins #9102. `fl[g] + fl[g]` passes one closure local twice in one expression: 7 + 7 = 14
//!   per iteration, 200 iterations: 2800.
module Conf;
public class Fl
  private _last: int := 0;
  public property [q: () -> int]: int => _last + q();
  public constructor() end constructor;
end Fl;

public class Program
  public static method Main(): int
    var t := 0;
    for var j in 0 ..< 200 do
      using var fl := new Fl();
      var g := lambda() => 7;
      t := t + fl[g] + fl[g];
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
