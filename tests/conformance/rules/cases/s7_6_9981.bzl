//! case: s7_6_9981
//! doc: spec
//! section: 7.6
//! title: `P?(Next())` over a property that is null skips the call and its argument
//! rule: when it is null the invocation does not occur and the argument expressions are not evaluated
//! cites: 7.6, 23.2
//! expect: run
//! output: -100
//!   0
//! notes: The null twin of s7_6_9980 (#9307): the getter yields null, the call does not occur, `Next` is never
//!   called and every Run yields -1: t = -100, Calls = 0.
module Conf;
public class Hx
  public static Calls: int := 0;
  private _q: ((int) -> int)?;
  public property P: ((int) -> int)? => null;
  public constructor()
    _q := lambda(x: int) => x + 2;
  end constructor;
  public static method Next(): int
    Hx.Calls := Hx.Calls + 1;
    return 1;
  end Next;
  public method Run(): int
    return P?(Next()) ?? -1;
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
