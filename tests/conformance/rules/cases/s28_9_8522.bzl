//! case: s28_9_8522
//! doc: spec
//! section: 28.9
//! title: a capturing closure kept in a field stays valid after the filler returns and the field
//!   changes
//! rule: Runtime management governs the *function value itself* — the heap-allocated closure
//!   object that backs it
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 598
//! notes: Pins #9061. A function value stored into an `Object` field is runtime-managed (§28.9):
//!   displacing it from the field, or the field's holder dying, must leave every other
//!   reference to it valid. `FillL` captures its local `j = k * 2` in the closure and keeps
//!   it; `Run` calls it before the field is overwritten by `H.Swap`: j + 1 with k = i mod 3
//!   gives 1, 3, 5 per cycle (9), 66 cycles = 594, and i = 198, 199 add 1 and 3: 598.
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
  public static method FillL(r: Hx, k: int)
    var j := k * 2;
    var f: () -> int := lambda() => j + 1;
    r.Keep(f);
  end FillL;
  public static method Swap(r: Hx)
    var g: () -> int := lambda() => 100;
    r.Keep(g);
  end Swap;
end H;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var h := new Hx();
      H.FillL(h, i mod 3);
      t := t + h.Run();
      H.Swap(h);
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
