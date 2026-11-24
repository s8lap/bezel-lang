//! case: s28_9_8519
//! doc: spec
//! section: 28.9
//! title: a function value read out of an Object field stays callable after the field is
//!   overwritten
//! rule: Runtime management governs the *function value itself* — the heap-allocated closure
//!   object that backs it
//! cites: 28.9, 8.1
//! expect: run
//! stdout: 1800 40000
//! notes: Pins #9061. A function value stored into an `Object` field is runtime-managed (§28.9):
//!   displacing it from the field, or the field's holder dying, must leave every other
//!   reference to it valid. `g` is read from the field, then `H.Swap` overwrites the field
//!   with a second closure, and `junk` allocates a third before `g` is called. g() = 9 each
//!   of 200 times: t = 1800; junk() = 200 each time: u = 40000.
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
  public static method Fill(r: Hx)
    var f: () -> int := lambda() => 9;
    r.Keep(f);
  end Fill;
  public static method Swap(r: Hx)
    var g: () -> int := lambda() => 100;
    r.Keep(g);
  end Swap;
end H;
public class Program
  public static method Main(): int
    var t := 0;
    var u := 0;
    for var i in 0 ..< 200 do
      var h := new Hx();
      H.Fill(h);
      var g := h.Get();
      H.Swap(h);
      var junk: () -> int := lambda() => 200;
      u := u + junk();
      if g is () -> int then t := t + (g as () -> int)!(); end if;
    end for;
    Console.WriteLine($"{t} {u}");
    return 0;
  end Main;
end Program;
