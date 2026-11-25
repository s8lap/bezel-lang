//! case: s28_14_8723
//! doc: spec
//! section: 28.14
//! title: a closure local used as an index argument can still be returned afterwards
//! rule: the callee receives the callback for the duration of the call only
//! cites: 28.14, 23.4
//! expect: run
//! stdout: 1400
//! notes: Pins #9102. `Mk` reads `fl[g]` with the call-bounded `g` and then returns `g`
//!   (capture-free) to its caller: each `h()` is 7, 200 times: 1400.
module Conf;
public class Fl
  private _last: int := 0;
  public property [q: () -> int]: int => _last + q();
  public constructor() end constructor;
end Fl;

public class Program
  public static method Mk(): () -> int
    using var fl := new Fl();
    var g := lambda() => 7;
    var x := fl[g];
    return g;
  end Mk;
  public static method Main(): int
    var t := 0;
    for var j in 0 ..< 200 do
      var h := Mk();
      t := t + h();
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
