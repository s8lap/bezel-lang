//! case: s7_6_9982
//! doc: spec
//! section: 7.6
//! title: `P?(1)` over a nullable function property whose getter reads a field leaves the lambda intact
//! rule: A function value of nullable function type may be invoked with `?(...)`
//! cites: 7.6, 23.2
//! expect: run
//! output: 700
//!   0
//! notes: The borrow twin of s7_6_9980 (#9307): the getter reads the field `_q` (x + 2), so each Run yields (1
//!   + 2) + (2 + 2) = 7 with the stored lambda still live for the second read: t = 700.
module Conf;
public class Hx
  public static Calls: int := 0;
  private _q: ((int) -> int)?;
  public property P: ((int) -> int)? => _q;
  public constructor()
    _q := lambda(x: int) => x + 2;
  end constructor;
  public method Run(): int
    return (P?(1) ?? 0) + (P?(2) ?? 0);
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
