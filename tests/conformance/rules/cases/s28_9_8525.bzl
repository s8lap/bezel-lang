//! case: s28_9_8525
//! doc: spec
//! section: 28.9
//! title: a function value returned from a try block stays callable after the finally overwrites
//!   its field
//! rule: Runtime management governs the *function value itself* — the heap-allocated closure
//!   object that backs it
//! cites: 28.9, 8.1
//! expect: run
//! stdout: 1800
//! notes: Pins #9061. A function value stored into an `Object` field is runtime-managed (§28.9):
//!   displacing it from the field, or the field's holder dying, must leave every other
//!   reference to it valid. `Take` returns the field's function value from a `try` whose
//!   `finally` overwrites the field (`H.Swap`); the returned value is the original closure,
//!   called by the caller afterwards: 9 each of 200 times, 1800.
module Conf;
public class Hx
  private _o: Object? := null;
  public constructor() end constructor;
  public method Keep(o: Object) _o := o; end Keep;
  public method Take(): Object?
    try
      return _o;
    finally
      H.Swap(this);
    end try;
  end Take;
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
    for var i in 0 ..< 200 do
      var h := new Hx();
      H.Fill(h);
      var g := h.Take();
      if g is () -> int then t := t + (g as () -> int)!(); end if;
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
