//! case: s28_14_8722
//! doc: spec
//! section: 28.14
//! title: a closure local passed as an index argument stays callable for the rest of its scope
//! rule: the callee receives the callback for the duration of the call only
//! cites: 28.14, 28.9, 23.4
//! expect: run
//! stdout: 8400
//! notes: Pins #9102. `g` is passed as the index argument (a call-bounded parameter) and then used
//!   again, directly and through a second closure `h` that calls it. Per iteration: fl[g] = 7,
//!   g() = 7, fl[g] + g() = 14, fl[g] + h() = 14: 42; 200 iterations: 8400.
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
      t := t + fl[g];
      t := t + g();
      t := t + fl[g] + g();
      var h := lambda() => g();
      t := t + fl[g] + h();
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
