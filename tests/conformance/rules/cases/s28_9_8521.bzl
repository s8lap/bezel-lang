//! case: s28_9_8521
//! doc: spec
//! section: 28.9
//! title: a function value that overwrites its own field and then throws unwinds to the caller's
//!   handler
//! rule: Runtime management governs the *function value itself* — the heap-allocated closure
//!   object that backs it
//! cites: 28.9, 8.1
//! expect: run
//! stdout: 1000
//! notes: Pins #9061. A function value stored into an `Object` field is runtime-managed (§28.9):
//!   displacing it from the field, or the field's holder dying, must leave every other
//!   reference to it valid. The closure kept in the field overwrites that field with a
//!   second closure and then throws (k >= 0 always holds); `Run` catches it and returns 5.
//!   200 iterations: 1000.
module Conf;
public class Hx
  private _o: Object? := null;
  public constructor() end constructor;
  public method Keep(o: Object) _o := o; end Keep;
  public method Run(): int
    try
      return (_o as (Hx) -> int)!(this);
    catch
      InvalidOperationException => return 5;
    end try;
    return 6;
  end Run;
end Hx;
public class H
  public static method FillT(r: Hx, k: int)
    var f: (Hx) -> int := lambda(x: Hx) => do x.Keep(lambda() => 0); if k >= 0 then throw new InvalidOperationException("x"); end if; return k; end;
    r.Keep(f);
  end FillT;
end H;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var h := new Hx();
      H.FillT(h, i mod 3);
      t := t + h.Run();
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
