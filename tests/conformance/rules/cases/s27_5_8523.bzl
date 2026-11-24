//! case: s27_5_8523
//! doc: spec
//! section: 27.5
//! title: a write to a captured local after the closure was kept in a field is visible to the
//!   kept closure
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 1000
//! notes: Pins #9061. A function value stored into an `Object` field is runtime-managed (§28.9):
//!   displacing it from the field, or the field's holder dying, must leave every other
//!   reference to it valid. `FillMut` keeps a closure over `j`, then assigns `j := 5` before
//!   returning; the kept closure reads the shared variable and returns 5 each of 200 times:
//!   1000.
module Conf;
public class Hx
  private _o: Object? := null;
  public constructor() end constructor;
  public method Keep(o: Object) _o := o; end Keep;
  public method Get(): Object? => _o;
  public method Run(): int
    if _o is () -> int then return (_o as () -> int)!(); end if;
    return 7;
  end Run;
end Hx;

public class H
  public static method FillMut(r: Hx)
    var j := 1;
    var f: () -> int := lambda() => j;
    r.Keep(f);
    j := 5;
  end FillMut;
end H;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var h := new Hx();
      H.FillMut(h);
      t := t + h.Run();
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
