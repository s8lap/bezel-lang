//! case: s7_6_9980
//! doc: spec
//! section: 7.6
//! title: `P?(1)` over an expression-bodied nullable function property invokes the lambda the getter yields
//! rule: A function value of nullable function type may be invoked with `?(...)`
//! cites: 7.6, 23.2
//! expect: run
//! output: 200
//!   0
//! notes: Pins #9307 (fix b00ba31aa). The rule corpus pins output only; the leak the fix closes is pinned by
//!   the ownership gtest rows. Each call yields 1 + 1 = 2; 100 calls: t = 200.
module Conf;
public class Hx
  public static Calls: int := 0;
  private _q: ((int) -> int)?;
  public property P: ((int) -> int)? => lambda(x: int) => x + 1;
  public constructor()
    _q := lambda(x: int) => x + 2;
  end constructor;
  public method Run(): int
    return P?(1) ?? 0;
  end Run;
end Hx;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 100 do
      var h := new Hx();
      t := t + h.Run();
    end for;
    Console.WriteLine($"{t}");
    Console.WriteLine($"{Hx.Calls}");
    return 0;
  end Main;
end Program;
