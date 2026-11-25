//! case: s28_14_8725
//! doc: spec
//! section: 28.14
//! title: a closure local used as an index argument can be captured by a returned closure
//!   afterwards
//! rule: the callee receives the callback for the duration of the call only
//! cites: 28.14, 23.4, 28.9
//! expect: run
//! stdout: 2800
//! notes: Pins #9102. After `fl[g]` (x = 7) the closure local `g` is captured by the closure `h`
//!   that `Mk` returns: h() = g() + x = 14, 200 times: 2800.
module Conf;
public class Fl
  private _last: int := 0;
  public property [q: () -> int]: int => _last + q();
  public constructor() end constructor;
end Fl;

public class Program
  public static method Mk(fl: Fl): () -> int
    var g := lambda() => 7;
    var x := fl[g];
    var h := lambda() => g() + x;
    return h;
  end Mk;
  public static method Main(): int
    var t := 0;
    using var fl := new Fl();
    for var j in 0 ..< 200 do
      var h := Mk(fl);
      t := t + h();
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
